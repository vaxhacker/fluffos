if(PACKAGE_HTTP)
    add_library(package_http STATIC http.cc http.h)
    target_link_libraries(package_http PRIVATE websockets event_extra_static)
endif()
