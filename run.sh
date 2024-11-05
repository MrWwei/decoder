export THIRD_PARTY=/home/mnt/wtwei/ThirdParty
# export LD_LIBRARY_PATH=/home/mnt/wtwei/ThirdParty/gdk_and_gtk:$THIRD_PARTY/mpp-develop/build/mpp:$THIRD_PARTY/rknpu2/examples/3rdparty/rga/RK3588/lib/Linux/aarch64:$THIRD_PARTY/FFmpeg-n6.0/install/lib:$THIRD_PARTY/opencv-4.5.4/lib:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH=$THIRD_PARTY/gdk_and_gtk:/home/user/mpp-develop/build/mpp:/home/user/librga/libs/Linux/gcc-aarch64:$THIRD_PARTY/FFmpeg-n6.0/install/lib:$THIRD_PARTY/opencv-4.5.4/lib:$LD_LIBRARY_PATH
# export LD_LIBRARY_PATH=/home/user/video_decoder-rk3588_hardware/libs:/home/user/video_decoder-rk3588_hardware/build:$LD_LIBRARY_PATH
./build/test_decoder rtsp_file.txt