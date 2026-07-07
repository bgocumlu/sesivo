# Packaging configuration for distributable desktop app artifacts.

if(APPLE AND JAM_BUILD_CLIENT)
    install(TARGETS client
        BUNDLE DESTINATION .
        COMPONENT client
    )

    set(CPACK_PACKAGE_NAME "sesivo")
    set(CPACK_PACKAGE_VENDOR "sesivo")
    set(CPACK_PACKAGE_VERSION "0.1.0")
    set(CPACK_PACKAGE_FILE_NAME "sesivo-${CPACK_PACKAGE_VERSION}")
    set(CPACK_COMPONENTS_ALL client)
    set(CPACK_INSTALL_CMAKE_PROJECTS
        "${CMAKE_BINARY_DIR};${CMAKE_PROJECT_NAME};client;/"
    )
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "sesivo")
    set(CPACK_DMG_FORMAT "UDZO")
    set(CPACK_DMG_DS_STORE_SETUP_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/dmg-layout.applescript"
    )

    include(CPack)
endif()
