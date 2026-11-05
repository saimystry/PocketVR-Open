// SPDX-License-Identifier: Apache-2.0
#include <jni.h>
#include <gtest/gtest.h>
#include <pocketvr/tracking.hpp>
#include <android/log.h>

namespace {
// gtest prints to stdout, which Android discards, so a failure would have no detail.
class LogFailures final : public testing::EmptyTestEventListener {
    void OnTestPartResult(const testing::TestPartResult &result) override {
        if (result.failed())
            __android_log_print(ANDROID_LOG_ERROR, "PocketVRTest", "%s:%d %s",
                                result.file_name() ? result.file_name() : "?", result.line_number(),
                                result.summary());
    }
    void OnTestEnd(const testing::TestInfo &test) override {
        if (test.result()->Failed())
            __android_log_print(ANDROID_LOG_ERROR, "PocketVRTest", "FAILED %s.%s",
                                test.test_suite_name(), test.name());
    }
};
} // namespace
extern "C" JNIEXPORT jint JNICALL Java_com_pocketvr_NativeTests_run(JNIEnv *, jclass) {
    int count = 1;
    char program[] = "PocketVR-Android-NativeTests";
    char *arguments[]{program, nullptr};
    testing::InitGoogleTest(&count, arguments);
    testing::UnitTest::GetInstance()->listeners().Append(new LogFailures);
    return RUN_ALL_TESTS();
}
extern "C" JNIEXPORT jdoubleArray JNICALL Java_com_pocketvr_NativeTests_sensorVector(
    JNIEnv *env, jclass, jdouble x, jdouble y, jdouble z, jint rotation) {
    try {
        const auto mapped = pocketvr::sensor_to_display({x, y, z}, static_cast<unsigned>(rotation));
        const jdouble values[]{mapped.x, mapped.y, mapped.z};
        const auto result = env->NewDoubleArray(3);
        if (result) env->SetDoubleArrayRegion(result, 0, 3, values);
        return result;
    } catch (...) {
        if (const auto type = env->FindClass("java/lang/IllegalArgumentException"))
            env->ThrowNew(type, "Invalid sensor mapping test input");
        return nullptr;
    }
}
