#include <chainbase/chainbase.hpp>
#include <boost/array.hpp>
#include <boost/version.hpp>

#include <cctype>
#include <fstream>
#include <iostream>
#include <string>

namespace chainbase {


    struct environment_check {
        environment_check() {
            memset(&compiler_version, 0, sizeof(compiler_version));
            // The Boost version rides inside the existing fixed-size buffer
            // rather than as a new field: this struct is persisted inside the
            // segment, so changing its layout would break the very check it
            // performs.  Boost.Interprocess's segment layout is not guaranteed
            // stable across Boost versions, so a mismatch must be refused.
#if defined(_MSC_VER)
            std::string ver = std::string("MSVC ") + _CRT_STRINGIZE(_MSC_VER);
#else
            std::string ver = __VERSION__;
#endif
            ver += " boost-";
            ver += BOOST_LIB_VERSION;
            // 255, not 256: the buffer must stay NUL-terminated so the
            // mismatch message below can read it back as a C string.
            memcpy(&compiler_version, ver.c_str(), std::min<size_t>(ver.size(), 255));
#ifndef NDEBUG
            debug = true;
#endif
#ifdef __APPLE__
            apple = true;
#endif
#ifdef WIN32
            windows = true;
#endif
        }

        friend bool operator==(const environment_check& a, const environment_check& b) {
            return std::make_tuple(a.compiler_version, a.debug, a.apple, a.windows)
                   ==
                   std::make_tuple(b.compiler_version, b.debug, b.apple, b.windows);
        }

        boost::array<char, 256> compiler_version;
        bool debug = false;
        bool apple = false;
        bool windows = false;
    };

    namespace {
        /// Recover the environment stamp straight out of the segment's bytes.
        ///
        /// When the state was written by a *different Boost*, the named-object
        /// index is itself unreadable, so find<environment_check>("environment")
        /// returns null and we cannot report what the state was built with --
        /// precisely the case the stamp exists to explain.  The struct is
        /// allocated at the head of the segment, so the string is recoverable by
        /// scanning the first few KiB for the "boost-" marker and taking the
        /// surrounding NUL-terminated run.  Returns empty if not found, which
        /// includes state written before the stamp existed.
        std::string scan_environment_stamp(const boost::filesystem::path& segment_file) {
            std::ifstream in(segment_file.string().c_str(), std::ios::binary);
            if (!in) {
                return std::string();
            }

            char head[4096];
            memset(head, 0, sizeof(head));
            in.read(head, sizeof(head));
            const size_t got = static_cast<size_t>(in.gcount());

            const std::string haystack(head, got);
            const size_t marker = haystack.find("boost-");
            if (marker == std::string::npos) {
                return std::string();
            }

            // Walk back to the start of the printable run, forward to its end.
            size_t begin = marker;
            while (begin > 0 && isprint(static_cast<unsigned char>(haystack[begin - 1]))) {
                --begin;
            }
            size_t end = marker;
            while (end < got && isprint(static_cast<unsigned char>(haystack[end]))) {
                ++end;
            }
            return haystack.substr(begin, end - begin);
        }
    }

    void database::open(const boost::filesystem::path& dir, uint32_t flags, size_t shared_file_size) {
        _read_lock_count.store(0);
        _write_lock_count.store(0);
        _undo_session_count.store(0);

        bool write = flags & database::read_write;

        if (!boost::filesystem::exists(dir)) {
            if (!write) {
                BOOST_THROW_EXCEPTION(std::runtime_error(std::string("database file not found at ") + dir.string()));
            }
        }

        boost::filesystem::create_directories(dir);
        if (_data_dir != dir) {
            close();
        }

        _data_dir = dir;
        auto abs_path = boost::filesystem::absolute(dir / "shared_memory.bin");

        if (boost::filesystem::exists(abs_path)) {
            if (write) {
                _file_size = boost::filesystem::file_size(abs_path);
                if (shared_file_size > _file_size) {
                    if (!boost::interprocess::managed_mapped_file::grow(
                            abs_path.generic_string().c_str(), shared_file_size - _file_size)
                    ) {
                        BOOST_THROW_EXCEPTION(std::runtime_error("could not grow database file to requested size."));
                    }
                    _file_size = shared_file_size;
                }

                _segment.reset(new boost::interprocess::managed_mapped_file(boost::interprocess::open_only,
                    abs_path.generic_string().c_str()
                ));

                // Post-grow validation: verify the segment's actual size
                // matches what we expect.  A mismatch means grow() extended
                // the file on disk but the managed_mapped_file metadata
                // wasn't updated (possible after a crash during a previous
                // resize cycle).
                auto actual_size = _segment->get_size();
                if (actual_size < _file_size) {
                    std::cerr << "WARNING: shared memory segment size (" << actual_size
                              << ") is smaller than expected (" << _file_size
                              << "). File may be corrupted from a previous incomplete resize."
                              << std::endl;
                }
            } else {
                _segment.reset(new boost::interprocess::managed_mapped_file(boost::interprocess::open_read_only,
                    abs_path.generic_string().c_str()
                ));
                _read_only = true;
                _file_size = shared_file_size;
            }

            auto env = _segment->find<environment_check>("environment");
            if (!env.first || !(*env.first == environment_check())) {
                std::string stored;
                if (env.first) {
                    stored = std::string(env.first->compiler_version.data());
                } else {
                    stored = scan_environment_stamp(abs_path);
                    if (stored.empty()) {
                        stored = "<unreadable -- predates this check, or a Boost too different to parse>";
                    }
                }
                std::string current(environment_check().compiler_version.data());
                BOOST_THROW_EXCEPTION(std::runtime_error(
                    "database was created by a different compiler, build, or operating system.\n"
                    "  state was built with: " + stored + "\n"
                    "  this binary uses:     " + current + "\n"
                    "Replay from the block log or import a snapshot to rebuild state."));
            }
        } else {
            _segment.reset(new boost::interprocess::managed_mapped_file(boost::interprocess::create_only,
                abs_path.generic_string().c_str(), shared_file_size
            ));
            _segment->find_or_construct<environment_check>("environment")();
            _file_size = shared_file_size;
        }

        if (write) {
            _flock = boost::interprocess::file_lock(abs_path.generic_string().c_str());
            if (!_flock.try_lock()) {
                BOOST_THROW_EXCEPTION(std::runtime_error("could not gain write access to the shared memory file"));
            }

            // Detect incomplete resize from a previous crash.
            // resize() writes this marker before the destructive grow/remap
            // and removes it after success.  If it survives, the shared
            // memory file may be in an inconsistent state.
            auto resize_marker = dir / "resize_in_progress";
            if (boost::filesystem::exists(resize_marker)) {
                std::cerr << "WARNING: resize_in_progress marker found at "
                          << resize_marker.string()
                          << ". Previous resize may have been interrupted. "
                          << "Shared memory may be corrupted."
                          << std::endl;
                // Don't throw here — let the caller (graphene::database::open)
                // decide whether to trigger recovery or continue.
            }
        }
    }

    void database::flush() {
        if (_segment) {
            _segment->flush();
        }
    }

    void database::close() {
        _segment.reset();
        _data_dir = boost::filesystem::path();
        _file_size = 0;
        _reserved_size = 0;
        _read_lock_count.store(0);
        _write_lock_count.store(0);
        _undo_session_count.store(0);
    }

    void database::wipe(const boost::filesystem::path& dir) {
        _segment.reset();
        boost::filesystem::remove_all(dir / "shared_memory.bin");
        // Clean up crash markers
        boost::system::error_code ec;
        boost::filesystem::remove(dir / "resize_in_progress", ec);
        _data_dir = boost::filesystem::path();
        _index_list.clear();
        _index_map.clear();
        _index_types.clear();
        _file_size = 0;
        _reserved_size = 0;
        _read_lock_count.store(0);
        _write_lock_count.store(0);
        _undo_session_count.store(0);
    }

    void database::resize(size_t new_shared_file_size) {
        if (_undo_session_count.load(std::memory_order_acquire) != 0) {
            BOOST_THROW_EXCEPTION(std::runtime_error("Cannot resize shared memory file while undo session is active"));
        }

        // Crash guard: write marker BEFORE any destructive operation.
        // If the process crashes during grow/remap, the marker survives
        // and triggers recovery on next startup.
        auto resize_marker = _data_dir / "resize_in_progress";
        { std::ofstream f(resize_marker.string()); f << new_shared_file_size; }

        // CRITICAL: Flush all dirty pages to disk before destroying the
        // mapping.  Without this, the OS may still have unwritten data
        // in the page cache; if grow() or the subsequent open() fails,
        // the on-disk file would be stale/inconsistent.
        _segment->flush();

        _segment.reset();

        try {
            open(_data_dir, database::read_write, new_shared_file_size);
        } catch (...) {
            // open() failed after the file may have been grown.
            // Leave the crash marker in place so the next startup
            // detects the incomplete resize and triggers recovery.
            std::cerr << "FATAL: shared memory resize failed. "
                      << "Crash marker left at " << resize_marker.string()
                      << " — restart will trigger recovery." << std::endl;
            throw;
        }

        _index_list.clear();
        _index_map.clear();

        for (auto& index_type: _index_types) {
            index_type->add_index(*this);
        }

        // Resize completed successfully — remove the crash marker.
        boost::system::error_code ec;
        boost::filesystem::remove(resize_marker, ec);
    }

    void database::set_require_locking(bool enable_require_locking) {
#ifdef CHAINBASE_CHECK_LOCKING
        _enable_require_locking = enable_require_locking;
#endif
    }

#ifdef CHAINBASE_CHECK_LOCKING

    void database::require_lock_fail(const char* method, const char* lock_type, const char* tname) const {
        std::string err_msg =
            "database::" + std::string(method) + " require_" + std::string(lock_type) + "_lock() failed on type " +
            std::string(tname);
        std::cerr << err_msg << std::endl;
        BOOST_THROW_EXCEPTION(std::runtime_error( err_msg ));
    }

#endif

    void database::undo() {
        for (auto& item : _index_list) {
            item->undo();
        }
    }

    void database::squash() {
        for (auto& item : _index_list) {
            item->squash();
        }
    }

    void database::commit(int64_t revision) {
        for (auto& item : _index_list) {
            item->commit(revision);
        }
    }

    void database::undo_all() {
        for (auto& item : _index_list) {
            item->undo_all();
            // Liveness signal for an external stall watchdog (see
            // graphene::chain::database::open): a corrupted index can make
            // undo() spin forever, freezing this loop with no progress.
            _undo_all_progress.fetch_add(1, std::memory_order_relaxed);
        }
    }

    database::session database::start_undo_session() {
        std::vector<boost::interprocess::unique_ptr<abstract_session>> sub_sessions;
        sub_sessions.reserve(_index_list.size());
        for (auto& item : _index_list) {
            sub_sessions.push_back(item->start_undo_session());
        }
        return session(std::move(sub_sessions), _undo_session_count);
    }

    int64_t database::revision() const {
        if (_index_list.size() == 0) {
            return -1;
        }
        return _index_list[0]->revision();
    }

    void database::set_revision(uint64_t revision) {
        CHAINBASE_REQUIRE_WRITE_LOCK("revision", uint64_t);
        for (auto i : _index_list) {
            i->set_revision(revision);
        }
    }

    std::size_t database::index_list_size() const {
        return _index_list.size();
    }

    auto database::index_list_begin() const -> index_list_type::const_iterator {
        return _index_list.begin();
    }

    auto database::index_list_end() const -> index_list_type::const_iterator {
        return _index_list.end();
    }

    auto database::segment_manager()
        -> decltype(std::declval<boost::interprocess::managed_mapped_file>().get_segment_manager())
    {
        return _segment->get_segment_manager();
    }

    size_t database::free_memory() const {
        return _segment->get_segment_manager()->get_free_memory();
    }

    void database::set_read_wait_micro(uint64_t value) {
        _read_wait_micro = value;
    }

    uint64_t database::read_wait_micro() const {
        return _read_wait_micro;
    }

    void database::set_max_read_wait_retries(uint32_t value) {
        _max_read_wait_retries = value;
    }

    uint32_t database::max_read_wait_retries() const {
        return _max_read_wait_retries;
    };

    void database::set_write_wait_micro(uint64_t value) {
        _write_wait_micro = value;
    }

    uint64_t database::write_wait_micro() const {
        return _write_wait_micro;
    }

    void database::set_max_write_wait_retries(uint32_t value) {
        _max_write_wait_retries = value;
    }

    uint32_t database::max_write_wait_retries() const {
        return _max_write_wait_retries;
    }

    size_t database::max_memory() const {
        return _file_size;
    }

    void database::set_reserved_memory(size_t value) {
        _reserved_size = value;
    }

    size_t database::reserved_memory() const {
        return _reserved_size;
    }

    void database::enter_operation() {
        std::unique_lock<std::mutex> lock(_resize_barrier_mutex);
        _resize_barrier_cv.wait(lock, [this]() {
            return !_resize_in_progress.load(std::memory_order_acquire);
        });
        _active_operations.fetch_add(1, std::memory_order_acq_rel);
    }

    void database::exit_operation() {
        auto prev = _active_operations.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1 && _resize_in_progress.load(std::memory_order_acquire)) {
            // Last operation completed while resize is waiting - wake it up
            std::lock_guard<std::mutex> lock(_resize_barrier_mutex);
            _resize_barrier_cv.notify_all();
        }
    }

    void database::begin_resize_barrier() {
        std::unique_lock<std::mutex> lock(_resize_barrier_mutex);
        _resize_in_progress.store(true, std::memory_order_release);
        // Wait until all in-flight operations have completed
        _resize_barrier_cv.wait(lock, [this]() {
            return _active_operations.load(std::memory_order_acquire) == 0;
        });
    }

    void database::end_resize_barrier() {
        {
            std::lock_guard<std::mutex> lock(_resize_barrier_mutex);
            _resize_in_progress.store(false, std::memory_order_release);
        }
        _resize_barrier_cv.notify_all();
    }

}  // namespace chainbase
