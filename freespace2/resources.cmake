

file(GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/macdebug-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:Freespace2>\n$<TARGET_FILE_DIR:Freespace2>"
)

if(WIN32)
    # Handling of windows resources+

    set(subpath res/win)

    set(RESOURCE_FILES
        ${subpath}/freespace.rc
    )

    set(ICONS
        ${subpath}/app_icon.ico
        ${subpath}/app_icon_glow.ico
        ${subpath}/dbg_icon.ico
        ${subpath}/goal_com.bmp
        ${subpath}/goal_fail.bmp
        ${subpath}/goal_inc.bmp
        ${subpath}/goal_none.bmp
        ${subpath}/goal_ord.bmp
        ${subpath}/V_app.ico
        ${subpath}/V_debug.ico
        ${subpath}/V_sse-d.ico
        ${subpath}/V_sse.ico
    )

    set(RESOURCES
        ${RESOURCE_FILES}
        ${ICONS}
    )

    target_sources(Freespace2 PRIVATE ${RESOURCES})

    source_group("Resources" FILES ${RESOURCE_FILES})
    source_group("Resources\\Icons" FILES ${ICONS})

    SET_SOURCE_FILES_PROPERTIES(${subpath}/freespace.rc PROPERTIES COMPILE_DEFINITIONS "_VC08")

    IF(FSO_INSTRUCTION_SET STREQUAL "SSE2" OR FSO_INSTRUCTION_SET STREQUAL "AVX")
    	set_property(SOURCE ${subpath}/freespace.rc APPEND_STRING PROPERTY COMPILE_DEFINITIONS ";_SSE2")
    ENDIF()

    set_property(SOURCE ${subpath}/freespace.rc APPEND_STRING PROPERTY
        COMPILE_DEFINITIONS ";VERSION_MAJOR=${FSO_VERSION_MAJOR};VERSION_MINOR=${FSO_VERSION_MINOR}")

    set_property(SOURCE ${subpath}/freespace.rc APPEND_STRING PROPERTY
        COMPILE_DEFINITIONS ";VERSION_BUILD=${FSO_VERSION_BUILD};VERSION_REVISION_NUM=${FSO_VERSION_REVISION_NUM}")

    set_property(SOURCE ${subpath}/freespace.rc APPEND_STRING PROPERTY
    COMPILE_DEFINITIONS ";FULL_VERSION_STRING=\"${FSO_FULL_VERSION_STRING}\"")

elseif(APPLE)
    # Handling of apple resources
    set(subpath res/mac)

    set(RESOURCES
        ${subpath}/FS2_Open.icns
        ${subpath}/English.lproj/InfoPlist.strings
    )

    target_sources(Freespace2 PRIVATE ${RESOURCES})

    set_source_files_properties(${subpath}/FS2_Open.icns MACOSX_PACKAGE_LOCATION Resources)
    set_source_files_properties(${subpath}/English.lproj/InfoPlist.strings MACOSX_PACKAGE_LOCATION Resources/English.lproj)

    source_group("Resources" FILES ${RESOURCES})

    set_target_properties(Freespace2 PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/${subpath}/Info.plist)

    # Also handle copying frameworks here
    add_custom_command(TARGET Freespace2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${FSO_MAC_FRAMEWORKS} $<TARGET_FILE_DIR:Freespace2>/../Frameworks
        COMMENT "Copying frameworks into bundle..."
    )
else()
    # No special resource handling required, add rules for new platforms here
endif()