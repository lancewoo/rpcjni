package com.senter.shotsync;

import org.bytedeco.javacpp.annotation.Platform;
import org.bytedeco.javacpp.annotation.Properties;
import org.bytedeco.javacpp.tools.Info;
import org.bytedeco.javacpp.tools.InfoMap;
import org.bytedeco.javacpp.tools.InfoMapper;

@Properties(
        // It's important that dependency headers go first, check https://github.com/bytedeco/javacpp/wiki/Mapping-Recipes#including-multiple-header-files
        value = @Platform(
                cinclude = {
                        "stc_rpc.h",
                },
                include = {
                        "RpcImpl.h"
                },
                // 指定的是加载时JNI库的名称和生成的C++代码的文件名，比如jniVsEngine对应libjniVsEngine.so
                library = "ShotSync"),
        target = "com.senter.shotsync.ShotSync"
)
public class NativeLibraryConfig implements InfoMapper {
    static {
        // Let Android take care of loading JNI libraries for us
//        System.setProperty("org.bytedeco.javacpp.loadLibraries", "false");
    }

    public void map(InfoMap infoMap) {
    }
}
