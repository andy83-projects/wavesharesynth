file(GLOB open303_sources "dsp/*.cpp")
idf_component_register(SRCS ${open303_sources} "acid_voice.cpp" INCLUDE_DIRS "." "dsp")
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -fno-strict-aliasing -Wno-unused-variable -Wno-unused-function)
