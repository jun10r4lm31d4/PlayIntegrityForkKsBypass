#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "zygisk.hpp"
#include "json.hpp"
#include "dobby.h"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "PIF/Native", __VA_ARGS__)

#define DEX_FILE_PATH "/data/adb/modules/playintegrityfix/classes.dex"

#define JSON_FILE_PATH "/data/adb/modules/playintegrityfix/pif.json"
#define CUSTOM_JSON_FILE_PATH "/data/adb/modules/playintegrityfix/custom.pif.json"
#define KEYBOX_FILE_PATH "/data/adb/modules/playintegrityfix/keybox.xml"

static int verboseLogs = 0;

static std::map<std::string, std::string> jsonProps;

typedef void (*T_Callback)(void *, const char *, const char *, uint32_t);

static std::map<void *, T_Callback> callbacks;

static void modify_callback(void *cookie, const char *name, const char *value, uint32_t serial) {
    if (cookie == nullptr || name == nullptr || value == nullptr || !callbacks.contains(cookie)) return;

    const char *oldValue = value;

    std::string prop(name);

    // Spoof specific property values
    if (prop == "init.svc.adbd") {
        value = "stopped";
    } else if (prop == "sys.usb.state") {
        value = "mtp";
    }

    if (jsonProps.count(prop)) {
        // Exact property match
        value = jsonProps[prop].c_str();
    } else {
        // Leading * wildcard property match
        for (const auto &p: jsonProps) {
            if (p.first.starts_with("*") && prop.ends_with(p.first.substr(1))) {
                value = p.second.c_str();
                break;
            }
        }
    }

    if (oldValue == value) {
        if (verboseLogs > 99) LOGD("[%s]: %s (unchanged)", name, oldValue);
    } else {
        LOGD("[%s]: %s -> %s", name, oldValue, value);
    }

    return callbacks[cookie](cookie, name, value, serial);
}

static void (*o_system_property_read_callback)(const prop_info *, T_Callback, void *);

static void my_system_property_read_callback(const prop_info *pi, T_Callback callback, void *cookie) {
    if (pi == nullptr || callback == nullptr || cookie == nullptr) {
        return o_system_property_read_callback(pi, callback, cookie);
    }
    callbacks[cookie] = callback;
    return o_system_property_read_callback(pi, modify_callback, cookie);
}

static void doHook() {
    void *handle = DobbySymbolResolver(nullptr, "__system_property_read_callback");
    if (handle == nullptr) {
        LOGD("Couldn't find '__system_property_read_callback' handle");
        return;
    }
    LOGD("Found '__system_property_read_callback' handle at %p", handle);
    DobbyHook(handle, reinterpret_cast<dobby_dummy_func_t>(my_system_property_read_callback),
        reinterpret_cast<dobby_dummy_func_t *>(&o_system_property_read_callback));
}

class PlayIntegrityFix : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        bool isGms = false, isGmsUnstable = false, isKeyAttestation = false;

        auto rawProcess = env->GetStringUTFChars(args->nice_name, nullptr);
        auto rawDir = env->GetStringUTFChars(args->app_data_dir, nullptr);

        // Prevent crash on apps with no data dir
        if (rawDir == nullptr) {
            env->ReleaseStringUTFChars(args->nice_name, rawProcess);
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        std::string_view process(rawProcess);
        std::string_view dir(rawDir);

        isGms = dir.ends_with("/com.google.android.gms");
        isGmsUnstable = process == "com.google.android.gms.unstable";
        // TODO hex3l: remove keyattestation app from checks
        isKeyAttestation = process == "io.github.vvb2060.keyattestation";

        env->ReleaseStringUTFChars(args->nice_name, rawProcess);
        env->ReleaseStringUTFChars(args->app_data_dir, rawDir);
        // TODO hex3l: remove keyattestation app from checks
        if (isKeyAttestation) LOGD("Found KeyAttestation APP");
        if (!isKeyAttestation) {
            if (!isGms) {
                api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
                return;
            }

            // We are in GMS now, force unmount
            api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);

            if (!isGmsUnstable) {
                api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
                return;
            }
        }

        std::vector<char> jsonVector;
        std::vector<char> xmlVector;
        long dexSize = 0, jsonSize = 0, xmlSize = 0;

        int fd = api->connectCompanion();

        read(fd, &dexSize, sizeof(long));
        read(fd, &jsonSize, sizeof(long));
        read(fd, &xmlSize, sizeof(long));

        if (dexSize < 1) {
            close(fd);
            LOGD("Couldn't read dex file");
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        if (jsonSize < 1) {
            close(fd);
            LOGD("Couldn't read json file");
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        if (xmlSize < 1) {
            close(fd);
            LOGD("Couldn't read xml file");
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        LOGD("Read from file descriptor for 'dex' -> %ld bytes", dexSize);
        LOGD("Read from file descriptor for 'json' -> %ld bytes", jsonSize);
        LOGD("Read from file descriptor for 'xml' -> %ld bytes", xmlSize);

        dexVector.resize(dexSize);
        read(fd, dexVector.data(), dexSize);

        jsonVector.resize(jsonSize);
        read(fd, jsonVector.data(), jsonSize);

        xmlVector.resize(xmlSize);
        read(fd, xmlVector.data(), xmlSize);

        close(fd);

        std::string jsonString(jsonVector.cbegin(), jsonVector.cend());
        json = nlohmann::json::parse(jsonString, nullptr, false, true);

        std::string xmlString(xmlVector.begin(), xmlVector.end());
        xml = xmlString;

        jsonVector.clear();
        jsonString.clear();
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (dexVector.empty() || json.empty() || xml.empty()) return;

        readJson();
        doHook();
        inject();

        dexVector.clear();
        json.clear();
        xml.clear();
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    std::vector<char> dexVector;
    std::string xml;
    nlohmann::json json;

    void readJson() {
        LOGD("JSON contains %d keys!", static_cast<int>(json.size()));

        // Verbose logging if verboseLogs with level number is present
        if (json.contains("verboseLogs")) {
            if (!json["verboseLogs"].is_null() && json["verboseLogs"].is_string() && json["verboseLogs"] != "") {
                verboseLogs = stoi(json["verboseLogs"].get<std::string>());
                if (verboseLogs > 0) LOGD("Verbose logging (level %d) enabled!", verboseLogs);
            } else {
                LOGD("Error parsing verboseLogs!");
            }
            json.erase("verboseLogs");
        }

        std::vector<std::string> eraseKeys;
        for (auto &jsonList: json.items()) {
            if (verboseLogs > 1) LOGD("Parsing %s", jsonList.key().c_str());
            if (jsonList.key().find_first_of("*.") != std::string::npos) {
                // Name contains . or * (wildcard) so assume real property name
                if (!jsonList.value().is_null() && jsonList.value().is_string()) {
                    if (jsonList.value() == "") {
                        LOGD("%s is empty, skipping", jsonList.key().c_str());
                    } else {
                        if (verboseLogs > 0) LOGD("Adding '%s' to properties list", jsonList.key().c_str());
                        jsonProps[jsonList.key()] = jsonList.value();
                    }
                } else {
                    LOGD("Error parsing %s!", jsonList.key().c_str());
                }
                eraseKeys.push_back(jsonList.key());
            }
        }
        // Remove properties from parsed JSON
        for (auto key: eraseKeys) {
            if (json.contains(key)) json.erase(key);
        }
    }

    void inject() {
        LOGD("JNI: Getting system classloader");
        auto clClass = env->FindClass("java/lang/ClassLoader");
        auto getSystemClassLoader = env->GetStaticMethodID(clClass, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
        auto systemClassLoader = env->CallStaticObjectMethod(clClass, getSystemClassLoader);

        LOGD("JNI: Creating module classloader");
        auto dexClClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        auto dexClInit = env->GetMethodID(dexClClass, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        auto buffer = env->NewDirectByteBuffer(dexVector.data(), static_cast<jlong>(dexVector.size()));
        auto dexCl = env->NewObject(dexClClass, dexClInit, buffer, systemClassLoader);

        LOGD("JNI: Loading module class");
        auto loadClass = env->GetMethodID(clClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        auto entryClassName = env->NewStringUTF("es.chiteroman.playintegrityfix.EntryPoint");
        auto entryClassObj = env->CallObjectMethod(dexCl, loadClass, entryClassName);

        auto entryClass = (jclass) entryClassObj;

        LOGD("JNI: Sending JSON");
        auto receiveJson = env->GetStaticMethodID(entryClass, "receiveJson", "(Ljava/lang/String;)V");
        auto jsonJavaStr = env->NewStringUTF(json.dump().c_str());
        env->CallStaticVoidMethod(entryClass, receiveJson, jsonJavaStr);

        LOGD("JNI: Sending XML");
        auto receiveXml = env->GetStaticMethodID(entryClass, "receiveXml", "(Ljava/lang/String;)V");
        auto xmlJavaString = env->NewStringUTF(xml.c_str());
        env->CallStaticVoidMethod(entryClass, receiveXml, xmlJavaString);

        LOGD("JNI: Calling init");
        auto entryInit = env->GetStaticMethodID(entryClass, "init", "(I)V");
        env->CallStaticVoidMethod(entryClass, entryInit, verboseLogs);
    }
};

static void companion(int fd) {
    long dexSize = 0, jsonSize = 0, xmlSize = 0;
    std::vector<char> dexVector, jsonVector, xmlVector;

    FILE *dex = fopen(DEX_FILE_PATH, "rb");

    if (dex) {
        fseek(dex, 0, SEEK_END);
        dexSize = ftell(dex);
        fseek(dex, 0, SEEK_SET);

        dexVector.resize(dexSize);
        fread(dexVector.data(), 1, dexSize, dex);

        fclose(dex);
    }

    FILE *json = fopen(CUSTOM_JSON_FILE_PATH, "r");
    if (!json)
        json = fopen(JSON_FILE_PATH, "r");

    if (json) {
        fseek(json, 0, SEEK_END);
        jsonSize = ftell(json);
        fseek(json, 0, SEEK_SET);

        jsonVector.resize(jsonSize);
        fread(jsonVector.data(), 1, jsonSize, json);

        fclose(json);
    }

    FILE *xml = fopen(KEYBOX_FILE_PATH, "r");

    if (xml) {
        fseek(xml, 0, SEEK_END);
        xmlSize = ftell(xml);
        fseek(xml, 0, SEEK_SET);

        xmlVector.resize(xmlSize);
        fread(xmlVector.data(), 1, xmlSize, xml);

        fclose(xml);
    }

    write(fd, &dexSize, sizeof(long));
    write(fd, &jsonSize, sizeof(long));
    write(fd, &xmlSize, sizeof(long));

    write(fd, dexVector.data(), dexSize);
    write(fd, jsonVector.data(), jsonSize);
    write(fd, xmlVector.data(), xmlSize);

    dexVector.clear();
    jsonVector.clear();
}

REGISTER_ZYGISK_MODULE(PlayIntegrityFix)

REGISTER_ZYGISK_COMPANION(companion)
