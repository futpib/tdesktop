cmake_minimum_required(VERSION 3.16)
project(test_qapp)

find_package(Qt6 REQUIRED COMPONENTS Widgets Quick QuickWidgets Svg OpenGL OpenGLWidgets DBus WaylandCompositor)

add_executable(test_qapp main.cpp)
target_link_libraries(test_qapp
    Qt6::Widgets
    Qt6::Quick
    Qt6::QuickWidgets
    Qt6::Svg
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::DBus
    Qt6::WaylandCompositor
)
