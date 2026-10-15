FILE(GLOB SRC *.cpp)

ADD_EXECUTABLE(shape2fx ${SRC})

TARGET_LINK_LIBRARIES(shape2fx PRIVATE NeL::misc NeL::3d)
NL_DEFAULT_PROPS(shape2fx "NeL, Tools, 3D: shape2fx")
NL_ADD_RUNTIME_FLAGS(shape2fx)
