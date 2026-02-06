export THIRD_PARTY=/work/ThirdParty
# export LD_LIBRARY_PATH=/home/user/mpp-develop/build/mpp:$THIRD_PARTY/rknpu2/examples/3rdparty/rga/RK3588/lib/Linux/aarch64:$THIRD_PARTY/FFmpeg-n6.0/install/lib:$THIRD_PARTY/opencv-4.5.4/lib:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH=$THIRD_PARTY/opencv-4.5.4_video/build2004/install/lib:$THIRD_PARTY/ZLMediaKit/install2004/lib:$THIRD_PARTY/FFmpeg-n6.0.1/install2004/lib:$LD_LIBRARY_PATH
# export LD_LIBRARY_PATH=$THIRD_PARTY/zlmediaKit/aarch64:$THIRD_PARTY/ffmpeg-4.3.8/install/lib:$THIRD_PARTY/opencv-4.5.4/build/install/lib:$LD_LIBRARY_PATH
# ./build/VideoDecoder rtsp_file.txt 
# ./build/VideoDecoder rtsp_file.txt 1 300 1 0
./build2004/VideoDecoder rtsp_file.txt  1 0 0 0
# ./build/VideoDecoder rtsp_file.txt 1
