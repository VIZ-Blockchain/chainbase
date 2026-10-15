#include <boost/test/unit_test.hpp>
#include <chainbase/chainbase.hpp>
#include <boost/multi_index/member.hpp>

namespace {
using namespace chainbase;
using namespace boost::multi_index;

// Throw from an actual stored object's assignment during generic_index::undo(),
// after mutation. No replacement session/index implementation or test hook.
struct undo_object : object<10, undo_object> {
    CHAINBASE_DEFAULT_CONSTRUCTOR(undo_object)
    undo_object(const undo_object& other) : id(other.id), value(other.value) {
        ++copies;
        if (copy_fail_at && copies == copy_fail_at) throw std::runtime_error("copy failed during merge");
    }
    undo_object& operator=(const undo_object& other) {
        id = other.id;
        value = other.value;
        ++assignments;
        if (fail && (fail_at == 0 || assignments == fail_at)) throw 42; // also exercise non-std exceptions
        return *this;
    }
    id_type id;
    int value = 0;
    static bool fail;
    static int fail_at;
    static int assignments;
    static int copies;
    static int copy_fail_at;
};
int undo_object::copies = 0;
int undo_object::copy_fail_at = 0;
bool undo_object::fail = false;
int undo_object::fail_at = 0;
int undo_object::assignments = 0;
using objects = multi_index_container<undo_object,
    indexed_by<ordered_unique<member<undo_object, undo_object::id_type, &undo_object::id>>>,
    allocator<undo_object>>;
using test_index = generic_index<objects>;

struct fixture {
    boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();
    boost::interprocess::managed_mapped_file memory;
    test_index first;
    test_index second;
    fixture() : memory(boost::interprocess::create_only, path.string().c_str(), 1024 * 1024),
        first(allocator<undo_object>(memory.get_segment_manager())),
        second(allocator<undo_object>(memory.get_segment_manager())) {
        undo_object::fail = false;
        undo_object::fail_at = 0;
        undo_object::assignments = 0;
        undo_object::copies = 0;
        undo_object::copy_fail_at = 0;
        first.emplace([](undo_object& o) { o.value = 1; });
        second.emplace([](undo_object& o) { o.value = 1; });
    }
    ~fixture() {
        undo_object::fail = false;
        boost::filesystem::remove(path);
    }
    void change(test_index& index) {
        index.modify(index.get(undo_object::id_type(0)), [](undo_object& o) { o.value = 2; });
    }
    std::vector<boost::interprocess::unique_ptr<abstract_session>> children() {
        std::vector<boost::interprocess::unique_ptr<abstract_session>> result;
        result.emplace_back(new session_impl<test_index::session>(first.start_undo_session()));
        result.emplace_back(new session_impl<test_index::session>(second.start_undo_session()));
        change(first);
        change(second);
        return result;
    }
};
}

BOOST_FIXTURE_TEST_CASE(failed_explicit_undo_is_not_retried, fixture) {
    auto session = first.start_undo_session();
    change(first);
    undo_object::fail = true;
    BOOST_CHECK_THROW(session.undo(), int);
    BOOST_CHECK_NO_THROW(session.undo());
    BOOST_CHECK_EQUAL(undo_object::assignments, 1);
    // A failed undo did not pop the state or claim rollback succeeded.
    BOOST_CHECK_EQUAL(first.revision(), 1);
}

BOOST_FIXTURE_TEST_CASE(failed_destructor_undo_during_unwind, fixture) {
    try {
        auto session = first.start_undo_session();
        change(first);
        undo_object::fail = true;
        throw std::runtime_error("original exception");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK_EQUAL(e.what(), "original exception");
    }
    BOOST_CHECK_EQUAL(undo_object::assignments, 1);
}

BOOST_FIXTURE_TEST_CASE(database_undo_attempts_each_child_once, fixture) {
    std::atomic<int32_t> count{0};
    {
        database::session owner(children(), count);
        BOOST_CHECK_EQUAL(count.load(), 1);
        undo_object::fail = true;
        BOOST_CHECK_THROW(owner.undo(), int);
        BOOST_CHECK_EQUAL(undo_object::assignments, 2);
        BOOST_CHECK_NO_THROW(owner.undo());
    }
    BOOST_CHECK_EQUAL(count.load(), 0);
    BOOST_CHECK_EQUAL(undo_object::assignments, 2);
}

BOOST_FIXTURE_TEST_CASE(database_destructor_cleans_children_during_unwind, fixture) {
    std::atomic<int32_t> count{0};
    try {
        database::session owner(children(), count);
        undo_object::fail = true;
        throw std::runtime_error("original exception");
    } catch (const std::runtime_error&) {}
    BOOST_CHECK_EQUAL(count.load(), 0);
    BOOST_CHECK_EQUAL(undo_object::assignments, 2);
}

BOOST_FIXTURE_TEST_CASE(database_failure_still_restores_healthy_child, fixture) {
    std::atomic<int32_t> count{0};
    {
        database::session owner(children(), count);
        undo_object::fail = true;
        undo_object::fail_at = 1;
        BOOST_CHECK_THROW(owner.undo(), int);
        BOOST_CHECK_EQUAL(second.get(undo_object::id_type(0)).value, 1);
        BOOST_CHECK_EQUAL(second.revision(), 0);
        BOOST_CHECK_EQUAL(first.revision(), 1);
    }
    BOOST_CHECK_EQUAL(count.load(), 0);
    BOOST_CHECK_EQUAL(undo_object::assignments, 2);
}

BOOST_FIXTURE_TEST_CASE(session_move_transfers_index_and_revision, fixture) {
    auto target = first.start_undo_session();
    change(first);
    // Give source and target distinct revisions to check both move operations.
    auto prior = second.start_undo_session();
    prior.push();
    auto source = second.start_undo_session();
    change(second);
    const auto revision = source.revision();
    auto moved = std::move(source);
    BOOST_CHECK_EQUAL(moved.revision(), revision);
    source.undo(); // moved-from objects own no rollback
    target = std::move(moved);
    moved.undo();
    target = std::move(target); // self-move retains the obligation
    BOOST_CHECK_EQUAL(first.get(undo_object::id_type(0)).value, 1);
    BOOST_CHECK_EQUAL(target.revision(), revision);
    target.undo();
    target.undo();
    BOOST_CHECK_EQUAL(second.get(undo_object::id_type(0)).value, 1);
    BOOST_CHECK_EQUAL(undo_object::assignments, 2);
}

BOOST_FIXTURE_TEST_CASE(failed_move_assignment_does_not_retry_target, fixture) {
    auto target = first.start_undo_session();
    change(first);
    auto source = second.start_undo_session();
    change(second);
    undo_object::fail = true;
    BOOST_CHECK_THROW(target = std::move(source), int);
    BOOST_CHECK_NO_THROW(target.undo());
    BOOST_CHECK_EQUAL(undo_object::assignments, 1);
    // Failed assignment did not consume the source's rollback ownership.
    undo_object::fail = false;
    source.undo();
    BOOST_CHECK_EQUAL(second.get(undo_object::id_type(0)).value, 1);
    BOOST_CHECK_EQUAL(undo_object::assignments, 2);
}

BOOST_FIXTURE_TEST_CASE(database_move_preserves_session_accounting, fixture) {
    std::atomic<int32_t> count{0};
    {
        database::session owner(children(), count);
        database::session moved(std::move(owner));
        // The existing counter tracks live wrapper objects, including moved-from.
        BOOST_CHECK_EQUAL(count.load(), 2);
        moved.undo();
        BOOST_CHECK_EQUAL(first.get(undo_object::id_type(0)).value, 1);
        BOOST_CHECK_EQUAL(second.get(undo_object::id_type(0)).value, 1);
    }
    BOOST_CHECK_EQUAL(count.load(), 0);
}

BOOST_FIXTURE_TEST_CASE(failed_destructor_with_throwing_diagnostic_stream, fixture) {
    struct throwing_buffer : std::streambuf {
        int_type overflow(int_type) override { throw std::runtime_error("logging failed"); }
    } buffer;
    auto* original = std::cerr.rdbuf(&buffer);
    const auto exceptions = std::cerr.exceptions();
    std::cerr.exceptions(std::ios::badbit | std::ios::failbit);
    {
        auto session = first.start_undo_session();
        change(first);
        undo_object::fail = true;
    }
    std::cerr.exceptions(std::ios::goodbit);
    std::cerr.clear();
    std::cerr.rdbuf(original);
    std::cerr.exceptions(exceptions);
    BOOST_CHECK_EQUAL(undo_object::assignments, 1);
}

BOOST_FIXTURE_TEST_CASE(database_push_and_squash_preserve_changes, fixture) {
    std::atomic<int32_t> count{0};
    {
        database::session owner(children(), count);
        owner.push();
        owner.undo(); // push has released the owner's rollback obligations
    }
    BOOST_CHECK_EQUAL(count.load(), 0);
    BOOST_CHECK_EQUAL(first.get(undo_object::id_type(0)).value, 2);
    BOOST_CHECK_EQUAL(second.get(undo_object::id_type(0)).value, 2);
    first.undo();
    second.undo();
    {
        auto outer_first = first.start_undo_session();
        auto outer_second = second.start_undo_session();
        {
            database::session inner(children(), count);
            inner.squash();
        }
        BOOST_CHECK_EQUAL(count.load(), 0);
        BOOST_CHECK_EQUAL(first.get(undo_object::id_type(0)).value, 2);
        BOOST_CHECK_EQUAL(second.get(undo_object::id_type(0)).value, 2);
    }
    BOOST_CHECK_EQUAL(first.get(undo_object::id_type(0)).value, 1);
    BOOST_CHECK_EQUAL(second.get(undo_object::id_type(0)).value, 1);
}

BOOST_FIXTURE_TEST_CASE(nested_failed_undo_is_not_retried_by_outer_session, fixture) {
    try {
        auto outer = first.start_undo_session();
        change(first);
        auto inner = first.start_undo_session();
        first.modify(first.get(undo_object::id_type(0)), [](undo_object& o) { o.value = 3; });
        undo_object::fail = true;
        throw std::runtime_error("original nested exception");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK_EQUAL(e.what(), "original nested exception");
    }
    BOOST_CHECK_EQUAL(undo_object::assignments, 1);
    BOOST_CHECK_EQUAL(first.revision(), 2);
}

BOOST_FIXTURE_TEST_CASE(nested_failed_explicit_undo_rejects_outer_revision, fixture) {
    auto outer = first.start_undo_session();
    change(first);
    auto inner = first.start_undo_session();
    first.modify(first.get(undo_object::id_type(0)), [](undo_object& o) { o.value = 3; });
    undo_object::fail = true;
    BOOST_CHECK_THROW(inner.undo(), int);
    BOOST_CHECK_THROW(outer.undo(), std::logic_error);
    BOOST_CHECK_NO_THROW(outer.undo());
    BOOST_CHECK_EQUAL(undo_object::assignments, 1);
    BOOST_CHECK_EQUAL(first.revision(), 2);
}

BOOST_FIXTURE_TEST_CASE(failed_destructor_with_failing_stream_sync, fixture) {
    struct failing_sync_buffer : std::streambuf {
        int_type overflow(int_type ch) override { return traits_type::not_eof(ch); }
        std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
        int sync() override { return -1; }
    } buffer;
    auto* original = std::cerr.rdbuf(&buffer);
    const auto exceptions = std::cerr.exceptions();
    std::cerr.exceptions(std::ios::badbit | std::ios::failbit);
    {
        auto session = first.start_undo_session();
        change(first);
        undo_object::fail = true;
    }
    std::cerr.exceptions(std::ios::goodbit);
    std::cerr.clear();
    std::cerr.rdbuf(original);
    std::cerr.exceptions(exceptions);
    BOOST_CHECK_EQUAL(undo_object::assignments, 1);
}

BOOST_FIXTURE_TEST_CASE(partial_squash_failure_does_not_undo_merged_history, fixture) {
    first.emplace([](undo_object& o) { o.value = 1; });
    bool caught = false;
    try {
        auto outer = first.start_undo_session();
        auto inner = first.start_undo_session();
        change(first);
        first.modify(first.get(undo_object::id_type(1)), [](undo_object& o) { o.value = 2; });
        undo_object::copies = 0;
        undo_object::copy_fail_at = 2; // one parent old_value inserted, second copy fails
        inner.squash();
    } catch (const std::runtime_error&) { caught = true; }
    BOOST_CHECK(caught);
    BOOST_CHECK_EQUAL(undo_object::copies, 2);
    BOOST_CHECK_EQUAL(undo_object::assignments, 0);
    BOOST_CHECK_EQUAL(first.revision(), 2);
    BOOST_CHECK_EQUAL(first.get(undo_object::id_type(0)).value, 2);
    BOOST_CHECK_EQUAL(first.get(undo_object::id_type(1)).value, 2);
}

BOOST_FIXTURE_TEST_CASE(nested_push_requires_resolving_top_revision, fixture) {
    auto outer = first.start_undo_session();
    change(first);
    {
        auto inner = first.start_undo_session();
        first.modify(first.get(undo_object::id_type(0)), [](undo_object& o) { o.value = 3; });
        inner.push();
    }
    first.undo(); // resolve the pushed inner revision before the outer owner
    outer.undo();
    BOOST_CHECK_EQUAL(first.get(undo_object::id_type(0)).value, 1);
    BOOST_CHECK_EQUAL(first.revision(), 0);
    auto another_outer = first.start_undo_session();
    {
        auto pushed = first.start_undo_session();
        pushed.push();
    }
    BOOST_CHECK_THROW(another_outer.undo(), std::logic_error);
    BOOST_CHECK_EQUAL(first.revision(), 2);
}

BOOST_FIXTURE_TEST_CASE(normal_session_lifecycle, fixture) {
    {
        auto session = first.start_undo_session();
        change(first);
    }
    BOOST_CHECK_EQUAL(first.get(undo_object::id_type(0)).value, 1);
    {
        auto session = first.start_undo_session();
        change(first);
        session.push();
    }
    BOOST_CHECK_EQUAL(first.get(undo_object::id_type(0)).value, 2);
    first.undo();
    BOOST_CHECK_EQUAL(first.get(undo_object::id_type(0)).value, 1);
    {
        auto outer = first.start_undo_session();
        change(first);
        {
            auto inner = first.start_undo_session();
            first.modify(first.get(undo_object::id_type(0)), [](undo_object& o) { o.value = 3; });
            inner.squash();
        }
        BOOST_CHECK_EQUAL(first.get(undo_object::id_type(0)).value, 3);
    }
    BOOST_CHECK_EQUAL(first.get(undo_object::id_type(0)).value, 1);
    BOOST_CHECK_EQUAL(first.revision(), 0);
}
