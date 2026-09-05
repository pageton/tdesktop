# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

# Generates an app-local qt.conf and a qt-plugins/ symlink tree next to each
# configuration's executable, so a non-wrapped binary can locate Qt plugins
# that live outside the base Qt installation. Without this, launching the
# binary directly fails to find e.g. the WebP image format codec (shipped as
# a separate qtimageformats package on Nix), which makes emoji sprites fail
# to decode and crashes the app on first emoji rendering.
function(generate_qt_conf target_name)
    cmake_parse_arguments(arg "" "PLUGINS_DIR" "EXTRA_PLUGINS" ${ARGN})

    if (NOT EXISTS "${arg_PLUGINS_DIR}")
        message(WARNING "Qt plugins directory not found: ${arg_PLUGINS_DIR}")
        return()
    endif()

    get_target_property(output_dir ${target_name} RUNTIME_OUTPUT_DIRECTORY)
    get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if (is_multi_config)
        set(directories)
        foreach(config ${CMAKE_CONFIGURATION_TYPES})
            list(APPEND directories "${output_dir}/${config}")
        endforeach()
    else()
        set(directories "${output_dir}")
    endif()

    foreach(directory IN LISTS directories)
        set(farm_dir "${directory}/qt-plugins")
        file(MAKE_DIRECTORY "${farm_dir}")

        file(GLOB_RECURSE plugin_files RELATIVE "${arg_PLUGINS_DIR}" "${arg_PLUGINS_DIR}/*")
        foreach(relative IN LISTS plugin_files)
            get_filename_component(category "${relative}" DIRECTORY)
            if (NOT "${category}" STREQUAL "")
                file(MAKE_DIRECTORY "${farm_dir}/${category}")
            endif()
            file(REMOVE "${farm_dir}/${relative}")
            file(CREATE_LINK "${arg_PLUGINS_DIR}/${relative}" "${farm_dir}/${relative}" SYMBOLIC)
        endforeach()

        foreach(plugin IN LISTS arg_EXTRA_PLUGINS)
            if (EXISTS "${plugin}")
                get_filename_component(category "${plugin}" DIRECTORY)
                get_filename_component(category "${category}" NAME)
                get_filename_component(name "${plugin}" NAME)
                file(MAKE_DIRECTORY "${farm_dir}/${category}")
                file(REMOVE "${farm_dir}/${category}/${name}")
                file(CREATE_LINK "${plugin}" "${farm_dir}/${category}/${name}" SYMBOLIC)
            endif()
        endforeach()

        file(CONFIGURE
            OUTPUT "${directory}/qt.conf"
            CONTENT "[Paths]\nPlugins = qt-plugins\n"
            @ONLY
        )
    endforeach()
endfunction()
