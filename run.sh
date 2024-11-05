export THIRD_PARTY=/home/ubuntu/ThirdParty
# export LD_LIBRARY_PATH=/home/user/mpp-develop/build/mpp:$THIRD_PARTY/rknpu2/examples/3rdparty/rga/RK3588/lib/Linux/aarch64:$THIRD_PARTY/FFmpeg-n6.0/install/lib:$THIRD_PARTY/opencv-4.5.4/lib:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH=$THIRD_PARTY/opencv-4.5.4/build/install/lib:$THIRD_PARTY/ZLMediaKit/install/lib:/home/ubuntu/wtwei/work/code/zlmediakit_ffmpeg/build:/home/ubuntu/wtwei/work/code/object_detect/build:$THIRD_PARTY/ffmpeg-4.3.8/install/lib:$LD_LIBRARY_PATH
# export LD_LIBRARY_PATH=$THIRD_PARTY/zlmediaKit/aarch64:$THIRD_PARTY/ffmpeg-4.3.8/install/lib:$THIRD_PARTY/opencv-4.5.4/build/install/lib:$LD_LIBRARY_PATH
./build/VideoDecoder rtsp_file.txt
