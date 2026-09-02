set(WWW_DIR "${CMAKE_CURRENT_LIST_DIR}/www")
set(WWW_FILES
    "${WWW_DIR}/index.html.gz"
    "${WWW_DIR}/app.js.gz"
    "${WWW_DIR}/app.css.gz")

foreach(f ${WWW_FILES})
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR
            "Missing ${f}.\n"
            "The web UI has not been built yet. Run:\n"
            "    python tools/build_web.py\n"
            "from the project root (it runs npm install/build in web/ and "
            "gzips the result into components/web_server/www).")
    endif()
endforeach()

idf_component_register(
    SRCS "src/web_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES json ulani_app tesserae net_provision
    EMBED_FILES ${WWW_FILES})
