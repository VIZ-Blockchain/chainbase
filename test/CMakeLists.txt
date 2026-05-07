file(GLOB UNIT_TESTS "*.cpp")

if (NOT Boost_USE_STATIC_LIBS)
    add_definitions(-DBOOST_TEST_DYN_LINK)
endif()

add_executable(chainbase_test ${UNIT_TESTS})
target_link_libraries(chainbase_test chainbase ${PLATFORM_SPECIFIC_LIBS} ${Boost_LIBRARIES})

# On Windows, ensure boost_exception is linked (auto-link may not find it)
if(WIN32)
    target_link_libraries(chainbase_test ${BOOST_ROOT}/lib/libboost_exception-vc142-mt-x64-1_84.lib)
endif()