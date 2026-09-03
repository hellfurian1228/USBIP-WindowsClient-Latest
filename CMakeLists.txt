cmake_minimum_required(VERSION 3.16)
project(USBIPClient VERSION 1.0.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(NOT DEFINED CMAKE_PREFIX_PATH AND NOT DEFINED Qt6_DIR)
    # Check default Qt install directory if not explicitly provided
    file(GLOB QT_CANDIDATE_DIRS LIST_DIRECTORIES true
        "C:/Qt/6.*/msvc2022_64"
        "C:/Qt/6.*/msvc2022_arm64"
        "C:/Qt/6.*/mingw_64"
    )
    if(QT_CANDIDATE_DIRS)
        list(GET QT_CANDIDATE_DIRS -1 LATEST_QT)
        list(APPEND CMAKE_PREFIX_PATH "${LATEST_QT}")
    endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

# --- usbip-win2 user-mode SDK (built from source to avoid ABI mismatches) ---
set(USBIP_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src/usbip_sdk")
file(GLOB USBIP_SDK_SOURCES CONFIGURE_DEPENDS
    "${USBIP_SDK_ROOT}/libusbip/src/*.c"
    "${USBIP_SDK_ROOT}/libusbip/src/*.cpp"
)

# Generate the message-table header referenced as <resources/messages.h>
if(WIN32)
    # The message compiler must run on the host build machine
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "ARM64")
        set(_MC_ARCH_HINTS "arm64" "x64" "x86")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_MC_ARCH_HINTS "x64" "x86")
    else()
        set(_MC_ARCH_HINTS "x86" "x64")
    endif()

    set(WIN_SDK_MC_PATHS "")
    foreach(_ARCH ${_MC_ARCH_HINTS})
        file(GLOB _PATHS "C:/Program Files (x86)/Windows Kits/10/bin/*/${_ARCH}")
        list(APPEND WIN_SDK_MC_PATHS ${_PATHS})
    endforeach()

    find_program(MC_EXECUTABLE mc
        HINTS
            ${WIN_SDK_MC_PATHS}
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x86"
        NO_DEFAULT_PATH
    )
    if(NOT MC_EXECUTABLE)
        find_program(MC_EXECUTABLE mc REQUIRED)
    endif()
else()
    find_program(MC_EXECUTABLE mc REQUIRED)
endif()

set(USBIP_SDK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/usbip_sdk")

add_custom_command(
    OUTPUT "${USBIP_SDK_GENERATED_DIR}/resources/messages.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${USBIP_SDK_GENERATED_DIR}/resources"
    COMMAND ${MC_EXECUTABLE} -h "${USBIP_SDK_GENERATED_DIR}/resources" -r "${USBIP_SDK_GENERATED_DIR}/resources"
        "${USBIP_SDK_ROOT}/include/resources/messages.mc"
    MAIN_DEPENDENCY "${USBIP_SDK_ROOT}/include/resources/messages.mc"
    COMMENT "Generating messages.h from messages.mc"
    VERBATIM
)

add_custom_target(usbip_sdk_messages DEPENDS "${USBIP_SDK_GENERATED_DIR}/resources/messages.h")

add_library(usbip_sdk STATIC ${USBIP_SDK_SOURCES})
add_dependencies(usbip_sdk usbip_sdk_messages)

target_include_directories(usbip_sdk PUBLIC
    "${USBIP_SDK_ROOT}/include"
    "${USBIP_SDK_ROOT}/libusbip"
    "${USBIP_SDK_GENERATED_DIR}"
)

# Private: libusbip/src contains an internal setupapi.h shim that would shadow
# the real Windows SDK <setupapi.h> for consumers if exposed publicly.
target_include_directories(usbip_sdk PRIVATE
    "${USBIP_SDK_ROOT}/libusbip/src"
)

target_compile_definitions(usbip_sdk PUBLIC
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    LIBUSBIP_STATIC
)

# dllspec.h defaults USBIP_API to dllimport unless USBIP_EXPORTS is defined; consumers
# must see the same definition too, otherwise they emit dllimport thunks that don't
# resolve against the dllexport symbols in this static library (LNK2019)
target_compile_definitions(usbip_sdk PUBLIC USBIP_EXPORTS)

target_link_libraries(usbip_sdk PRIVATE
    ws2_32
    setupapi
    cfgmgr32
)

add_executable(USBIPClient
    main.cpp
    mainwindow.cpp
    mainwindow.h
    logwindow.cpp
    logwindow.h
    driverinstaller.cpp
    driverinstaller.h
    nsddiscoverymanager.cpp
    nsddiscoverymanager.h
    wifimanager.cpp
    wifimanager.h
    src/transport/hybrid_udp_transport.cpp
    src/transport/hybrid_udp_transport.h
    src/usbip.rc
)

target_link_libraries(USBIPClient PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    usbip_sdk
    setupapi
    newdev
    wlanapi
    ole32
)

target_include_directories(USBIPClient PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Self-elevation is handled programmatically in main.cpp, so no UAC manifest is needed here.

set_target_properties(USBIPClient PROPERTIES
    WIN32_EXECUTABLE ON
)

# Deploy Qt dependencies using windeployqt
find_program(WINDEPLOYQT_EXECUTABLE windeployqt
    HINTS
        "${Qt6_DIR}/../../../bin"
        "${Qt6_DIR}/../../bin"
        "${CMAKE_PREFIX_PATH}/bin"
        "C:/Qt/6.11.2/msvc2022_64/bin"
)

if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET USBIPClient POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            --no-compiler-runtime
            --verbose 0
            "$<TARGET_FILE:USBIPClient>"
    )
endif()

# Deploy the pre-signed kernel driver package next to the executable
add_custom_command(TARGET USBIPClient POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Drivers"
        "$<TARGET_FILE_DIR:USBIPClient>/Drivers"
)

# Copy usb.ids to build directory
add_custom_command(TARGET USBIPClient POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/usb.ids"
        "$<TARGET_FILE_DIR:USBIPClient>/usb.ids"
)

# --- Installation and Packaging Configuration ---

# Install target executable
install(TARGETS USBIPClient RUNTIME DESTINATION .)

# Install usb.ids
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/usb.ids" DESTINATION .)

# Deploy Qt dependencies using windeployqt
if(WINDEPLOYQT_EXECUTABLE)
    install(CODE "
        execute_process(
            COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                    --no-compiler-runtime
                    --verbose 0
                    \"\${CMAKE_INSTALL_PREFIX}/USBIPClient.exe\"
        )
    ")
endif()

# Install MSVC runtime libraries
include(InstallRequiredSystemLibraries)
if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION .)
endif()

# CPack Configuration
set(CPACK_PACKAGE_NAME "USBIP Client")
set(CPACK_PACKAGE_VENDOR "Tyler Long")
set(CPACK_PACKAGE_VERSION "1.0.5")
set(CPACK_GENERATOR "NSIS")

# NSIS Specifics
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "USBIP Client")
set(CPACK_NSIS_DISPLAY_NAME "USBIP Client")
set(CPACK_NSIS_PACKAGE_NAME "USBIP Client")
set(CPACK_NSIS_INSTALLED_ICON_NAME "USBIPClient.exe")
set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/src/usbip.ico")

# Create desktop shortcut
set(CPACK_NSIS_CREATE_ICONS_EXTRA "  CreateShortCut \\\"$DESKTOP\\\\USBIP Client.lnk\\\" \\\"$INSTDIR\\\\USBIPClient.exe\\\"")
set(CPACK_NSIS_DELETE_ICONS_EXTRA "  Delete \\\"$DESKTOP\\\\USBIP Client.lnk\\\"")

include(CPack)