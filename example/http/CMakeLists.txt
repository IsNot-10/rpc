set(HTTP_LIST
    HttpContext.cc
    HttpRequest.cc
    HttpResponse.cc
    HttpServer.cc
    RouteTable.cc
    Router.cc
    main.cc
)

add_executable(HttpServer ${HTTP_LIST})

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/example/http)

target_link_libraries(HttpServer mymuduo)
