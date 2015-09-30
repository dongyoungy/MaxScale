add_executable(test_parser test_parser.c)
target_link_libraries(test_parser parser)
add_test(Internal-TestParser test_parser)

