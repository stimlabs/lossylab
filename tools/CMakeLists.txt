add_executable(lossylab-info lossylab_info.cpp)
target_link_libraries(lossylab-info PRIVATE lossylab::lossylab)
lossylab_set_warnings(lossylab-info)
