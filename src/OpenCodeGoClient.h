#pragma once

// OpenCode Go 用量查询客户端
// 接口: GET https://opencode.ai/zen/go/v1/usage
// Authorization: Bearer <API Key>（Anthropic 兼容 Key）
// User-Agent 保持 cc-switch/1.0，与 CC Switch 预设一致。
// 底层 HTTPS/HTTP 读取解析由 sd2-common 的 sd2::Https 提供。

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD2Common.h>

#include "config.h"
#include "cert.h"

struct OpenCodeGoWindow {
    bool present = false;  // usage.<key> 是否存在
    bool valid = true;     // status != "invalid"
    int percent = 0;       // 已用百分比
    String status;
    String resetsAt;       // ISO8601 时间

    // 与 cc-switch 提取器一致：剩余 = max(100 - percent, 0)
    int remaining() const {
        int r = 100 - percent;
        return r > 0 ? r : 0;
    }
};

struct OpenCodeGoUsage {
    bool ok = false;       // 请求 + 解析是否成功
    int http_code = 0;
    OpenCodeGoWindow rolling;  // 5 小时
    OpenCodeGoWindow weekly;   // 周额度
    OpenCodeGoWindow monthly;  // 月额度
    String error;              // 中文错误描述
};

static sd2::Https https(GTS_ROOT_R4_PEM, VERIFY_TLS_CERT);

#define OPENCODE_API_HOST "opencode.ai"

static void parseWindow(const JsonObject &obj, OpenCodeGoWindow &out) {
    if (obj.isNull())
        return;
    out.present = true;
    out.status = obj["status"] | "";
    out.percent = obj["percent"] | 0;
    out.resetsAt = obj["resetsAt"] | "";
    out.valid = out.status != "invalid";
}

static bool fetchOpenCodeGoUsage(OpenCodeGoUsage &out, uint32_t timeout_ms = 15000) {
    out = OpenCodeGoUsage();

    sd2::HttpResponse resp;
    String error;
    if (!https.get(OPENCODE_API_HOST, "/zen/go/v1/usage",
                   OPENCODE_GO_API_KEY, "cc-switch/1.0",
                   resp, error, timeout_ms)) {
        out.error = error;
        return false;
    }
    out.http_code = resp.httpCode;
    Serial.printf("HTTP %d, body %u B\n",
                  out.http_code, (unsigned)resp.body.length());

    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, resp.body);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        out.error = "Response error";
        return false;
    }

    if (out.http_code == 200) {
        JsonObject usage = doc["usage"].as<JsonObject>();
        if (!usage.isNull()) {
            parseWindow(usage["rolling"].as<JsonObject>(), out.rolling);
            parseWindow(usage["weekly"].as<JsonObject>(), out.weekly);
            parseWindow(usage["monthly"].as<JsonObject>(), out.monthly);
        }

        if (!out.rolling.present && !out.weekly.present && !out.monthly.present) {
            out.error = "Response error";
            return false;
        }
        out.ok = true;
        return true;
    }

    String apiErr = doc["error"]["message"] | "";
    if (apiErr.length() > 0)
        Serial.printf("API error: %s\n", apiErr.c_str());

    // 401 = Key 无效；403 = Key 有效但没有 Go 订阅
    if (out.http_code == 401) {
        out.error = "Bad API key";
    } else if (out.http_code == 403) {
        out.error = "No Go plan";
    } else if (out.http_code == 429) {
        out.error = "HTTP 429";
    } else if (out.http_code >= 500) {
        out.error = "Server error";
    } else {
        out.error = "HTTP " + String(out.http_code);
    }
    return false;
}
