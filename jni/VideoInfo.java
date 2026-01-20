package cn.xtkj.jni.capture.data;

/**
 * @author htchen
 * @version 1.0
 * @ClassName: VideoInfo
 * @date 2026年01月19日 16:27:16
 */
public class VideoInfo {
    private int frameRate;//帧率
    private int bitrate;//码流（比特率）
    private int frameLen;//总帧长（未跳帧情况下，适用于视频文件）

    public int getFrameRate() {
        return frameRate;
    }

    public void setFrameRate(int frameRate) {
        this.frameRate = frameRate;
    }

    public int getBitrate() {
        return bitrate;
    }

    public void setBitrate(int bitrate) {
        this.bitrate = bitrate;
    }

    public int getFrameLen() {
        return frameLen;
    }

    public void setFrameLen(int frameLen) {
        this.frameLen = frameLen;
    }
}