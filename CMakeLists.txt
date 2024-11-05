cmake_minimum_required(VERSION 3.4.1)

project(VideoDecoder)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# skip 3rd-party lib dependencies
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-shlib-undefined")
set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -fPIC -O0 -Wfatal-errors -pthread -w -g")


set(ThirdParty $ENV{THIRD_PARTY})

if (CMAKE_C_COMPILER MATCHES "aarch64")
  set(LIB_ARCH aarch64)
else()
  set(LIB_ARCH armhf)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/include/utils)
# opencv
add_library(opencv_lib SHARED IMPORTED)
set_target_properties( opencv_lib PROPERTIES IMPORTED_LOCATION ${ThirdParty}/opencv-4.5.4/build/install/lib/libopencv_world.so )
include_directories(${ThirdParty}/opencv-4.5.4/build/install/include)
#rga
# set(RGA_LIB ${ThirdParty}/librga/libs/Linux/gcc-aarch64/librga.so)
# include_directories( ${ThirdParty}/librga/include)

# 设置ffmpeg依赖库及头文件所在目录，并存进指定变量                                                                      
set(ffmpeg_libs_DIR ${ThirdParty}/ffmpeg-4.3.8/install/lib)                                                              
set(ffmpeg_headers_DIR ${ThirdParty}/ffmpeg-4.3.8/install/include)                                                       
#对于find_package找不到的外部依赖库，可以用add_library添加                                                              
# SHARED表示添加的是动态库                                                                                              
# IMPORTED表示是引入已经存在的动态库                                                                                    
add_library( avcodec SHARED IMPORTED)                                                                                   
add_library( avfilter SHARED IMPORTED )                                                                                 
add_library( swresample SHARED IMPORTED )                                                                               
add_library( swscale SHARED IMPORTED )                                                                                  
add_library( avformat SHARED IMPORTED )                                                                                 
add_library( avutil SHARED IMPORTED )                                                                                   
# #指定所添加依赖库的导入路径                                                                                           
set_target_properties( avcodec PROPERTIES IMPORTED_LOCATION ${ffmpeg_libs_DIR}/libavcodec.so )                          
set_target_properties( avfilter PROPERTIES IMPORTED_LOCATION ${ffmpeg_libs_DIR}/libavfilter.so )                        
set_target_properties( swresample PROPERTIES IMPORTED_LOCATION ${ffmpeg_libs_DIR}/libswresample.so )                    
set_target_properties( swscale PROPERTIES IMPORTED_LOCATION ${ffmpeg_libs_DIR}/libswscale.so )                          
set_target_properties( avformat PROPERTIES IMPORTED_LOCATION ${ffmpeg_libs_DIR}/libavformat.so )
set_target_properties( avutil PROPERTIES IMPORTED_LOCATION ${ffmpeg_libs_DIR}/libavutil.so )


# 添加头文件路径到编译器的头文件搜索路径下，多个路径以空格分隔
include_directories( ${ffmpeg_headers_DIR} )
link_directories(${ffmpeg_libs_DIR})

# zlmediakit
set(ZLMEDIAKIT_PATH ${ThirdParty}/ZLMediaKit/install)
include_directories(${ZLMEDIAKIT_PATH}/include)
set(ZLMEDIAKIT_LIBS ${ZLMEDIAKIT_PATH}/lib/libmk_api.so)

aux_source_directory(${PROJECT_SOURCE_DIR}/src SRC_DIR)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/utils SRC_DIR)
set(sdk_target_name videoDecoder_ubuntu_SDK_V2.4)
add_library(${sdk_target_name} SHARED ${SRC_DIR})
target_link_libraries(${sdk_target_name}
     opencv_lib
     ${ZLMEDIAKIT_LIBS}
     avcodec                                                                                                                 
     avfilter                                                                                                                
     swresample                                                                                                        
     swscale                                                                                                                 
     avformat                                                                                                                
     avutil 
     pthread
)
add_executable(${PROJECT_NAME}  main.cpp)
target_link_libraries(${PROJECT_NAME}
    #  opencv_lib
     ${sdk_target_name}
)

# include_directories(${project_includes})
###############################jni#########################################
include_directories(${ThirdParty}/jdk1.8.0_381/include)
include_directories(${ThirdParty}/jdk1.8.0_381/include/linux)

set(jni_name_rtsp videoDecoder_ubuntu_SDK2.4_JNI_V2.4)
add_library(${jni_name_rtsp} SHARED
jni/cn_xtkj_jni_capture_VideoDecoder.cpp
)
# link the new hello_library target with the hello_binary target
target_link_libraries( ${jni_name_rtsp}
   PRIVATE 
   ${sdk_target_name}
)