add_library(hog_exec_common_objects OBJECT status.cc)
target_link_libraries(
    hog_exec_common_objects
    hogd_interface)
add_library(hog_exec_common STATIC $<TARGET_OBJECTS:hog_exec_common_objects>)
