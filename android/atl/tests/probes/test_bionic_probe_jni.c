#include <jni.h>
#include <string.h>

extern int __system_property_get(const char *name, char *value);

JNIEXPORT jstring JNICALL
Java_com_example_demo_1application_MainActivity_nativeGetBionicSdkProperty(JNIEnv *env, jclass clazz)
{
	(void)clazz;
	char value[92];
	memset(value, 0, sizeof(value));
	if (__system_property_get("ro.build.version.sdk", value) > 0) {
		return (*env)->NewStringUTF(env, value);
	}
	return (*env)->NewStringUTF(env, "");
}
