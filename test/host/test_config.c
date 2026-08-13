#include "app_config.h"
#include "gpio_rules.h"
#include <cJSON.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails, total;
#define CHECK(cond, ...) do{ total++; if(!(cond)){ fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } }while(0)

static esp_err_t apply(const char *json, app_config_t *cfg, char *err, size_t len){
    err[0]='\0';
    return app_config_from_json(json, cfg, err, len);
}

int main(void){
    char err[128];

    printf("defaults\n");
    app_config_t d; app_config_defaults(&d);
    CHECK(app_config_validate(&d, err, sizeof err)==ESP_OK, "defaults must validate: %s", err);
    CHECK(d.lock.gpio==2 && d.lock.unlock_ms==3000, "lock defaults wrong");
    CHECK(d.nfc.spi_sck==18 && d.nfc.spi_cs==5, "spi defaults wrong");

    printf("group id parsing\n");
    uint8_t gid[16];
    CHECK(app_config_parse_group_id("00112233445566778899AABBCCDDEEFF", gid, 16)==ESP_OK, "valid hex rejected");
    CHECK(gid[0]==0x00 && gid[1]==0x11 && gid[15]==0xFF, "hex decoded wrong: %02X %02X %02X", gid[0],gid[1],gid[15]);
    CHECK(app_config_parse_group_id("00112233445566778899AABBCCDDEEFG", gid, 16)!=ESP_OK, "non-hex accepted");
    CHECK(app_config_parse_group_id("0011", gid, 16)!=ESP_OK, "short string accepted");

    printf("pin rules (esp32)\n");
    CHECK(gpio_rules_is_restricted(6) && gpio_rules_is_restricted(11), "flash pins not restricted");
    CHECK(!gpio_rules_is_restricted(18), "GPIO18 wrongly restricted");
    CHECK(gpio_rules_is_strapping(0) && gpio_rules_is_strapping(12), "strapping pins missing");
    CHECK(gpio_rules_reject_reason(34, true)!=NULL, "input-only pin accepted as output");
    CHECK(gpio_rules_reject_reason(34, false)==NULL, "input-only pin rejected as input");
    CHECK(gpio_rules_reject_reason(18, true)==NULL, "GPIO18 rejected: %s", gpio_rules_reject_reason(18,true));

    printf("validation rejects bad configs\n");
    app_config_t c;
    struct { const char *json; const char *why; } bad[] = {
      {"{\"lock\":{\"gpio\":6}}",                 "flash pin as lock output"},
      {"{\"lock\":{\"gpio\":34}}",                "input-only pin as lock output"},
      {"{\"lock\":{\"gpio\":18}}",                "lock pin colliding with SPI SCK"},
      {"{\"lock\":{\"unlock_ms\":10}}",           "unlock too short"},
      {"{\"lock\":{\"unlock_ms\":99999}}",        "unlock too long"},
      {"{\"device\":{\"group_id\":\"abc\"}}",     "short group id"},
      {"{\"nfc\":{\"spi_freq_hz\":50}}",          "spi clock too low"},
      {"{\"nfc\":{\"spi_host\":9}}",              "bad spi host"},
      {"{\"nfc\":{\"bus\":\"i2c\"}}",             "i2c without sda/scl"},
      {"{\"net\":{\"ap_password\":\"short\"}}",   "ap password under 8 chars"},
      {"{\"nfc\":{\"spi_cs\":19}}",               "cs colliding with miso"},
      {"not json",                                "malformed json"},
    };
    for (size_t i=0;i<sizeof bad/sizeof bad[0];i++){
        app_config_defaults(&c);
        CHECK(apply(bad[i].json,&c,err,sizeof err)!=ESP_OK, "accepted %s", bad[i].why);
    }

    printf("validation accepts good configs\n");
    app_config_defaults(&c);
    CHECK(apply("{\"lock\":{\"gpio\":4,\"active_low\":true,\"unlock_ms\":1500}}",&c,err,sizeof err)==ESP_OK, "good lock rejected: %s", err);
    CHECK(c.lock.gpio==4 && c.lock.active_low && c.lock.unlock_ms==1500, "lock not applied");
    app_config_defaults(&c);
    CHECK(apply("{\"nfc\":{\"chip\":\"pn532\",\"bus\":\"spi\",\"spi_sck\":14,\"spi_miso\":12,\"spi_mosi\":13,\"spi_cs\":15,\"irq_pin\":27,\"rst_pin\":26}}",&c,err,sizeof err)==ESP_OK, "hspi pinout rejected: %s", err);
    CHECK(c.nfc.chip==NFC_CHIP_PN532 && c.nfc.spi_sck==14 && c.nfc.rst_pin==26, "nfc not applied");
    app_config_defaults(&c);
    CHECK(apply("{\"nfc\":{\"bus\":\"i2c\",\"i2c_sda\":21,\"i2c_scl\":22,\"i2c_addr\":36}}",&c,err,sizeof err)==ESP_OK, "i2c rejected: %s", err);

    printf("partial updates behave like a patch\n");
    app_config_defaults(&c);
    snprintf(c.net.password, sizeof c.net.password, "secret-wifi");
    CHECK(apply("{\"net\":{\"ssid\":\"home\",\"password\":\"\"}}",&c,err,sizeof err)==ESP_OK, "patch rejected: %s", err);
    CHECK(strcmp(c.net.ssid,"home")==0, "ssid not set");
    CHECK(strcmp(c.net.password,"secret-wifi")==0, "empty password erased the stored one");
    CHECK(c.lock.gpio==2, "untouched field changed");
    CHECK(apply("{\"net\":{\"password\":\"new-secret\"}}",&c,err,sizeof err)==ESP_OK, "password change rejected");
    CHECK(strcmp(c.net.password,"new-secret")==0, "password not updated");

    printf("mqtt validation\n");
    struct { const char *json; const char *why; } bad_mqtt[] = {
      {"{\"mqtt\":{\"enabled\":true}}",                                        "enabled with no broker"},
      {"{\"mqtt\":{\"enabled\":true,\"broker\":\"b\",\"port\":0}}",          "port 0"},
      {"{\"mqtt\":{\"enabled\":true,\"broker\":\"b\",\"port\":70000}}",      "port above 65535"},
      {"{\"mqtt\":{\"enabled\":true,\"broker\":\"b\",\"base_topic\":\"a/\"}}",  "base topic with trailing slash"},
      {"{\"mqtt\":{\"enabled\":true,\"broker\":\"b\",\"base_topic\":\"a/#\"}}",  "base topic with wildcard"},
      {"{\"mqtt\":{\"enabled\":true,\"broker\":\"b\",\"client_id\":\"\"}}",     "empty client id"},
    };
    for (size_t i=0;i<sizeof bad_mqtt/sizeof bad_mqtt[0];i++){
        app_config_defaults(&c);
        CHECK(apply(bad_mqtt[i].json,&c,err,sizeof err)!=ESP_OK, "accepted %s", bad_mqtt[i].why);
    }
    /* Everything above is only checked when MQTT is switched on. */
    app_config_defaults(&c);
    CHECK(apply("{\"mqtt\":{\"enabled\":false,\"broker\":\"\"}}",&c,err,sizeof err)==ESP_OK, "disabled mqtt validated as if enabled: %s", err);
    app_config_defaults(&c);
    CHECK(apply("{\"mqtt\":{\"enabled\":true,\"broker\":\"10.0.0.5\",\"port\":8883,\"use_ssl\":true,\"base_topic\":\"home/door\"}}",&c,err,sizeof err)==ESP_OK, "valid mqtt rejected: %s", err);
    CHECK(c.mqtt.enabled && c.mqtt.port==8883 && c.mqtt.use_ssl, "mqtt fields not applied");

    printf("mqtt topics\n");
    char topic[96];
    app_config_mqtt_topic(&c.mqtt, "lock/set", topic, sizeof topic);
    CHECK(strcmp(topic,"home/door/lock/set")==0, "topic built wrong: %s", topic);

    printf("mqtt password is a secret too\n");
    app_config_defaults(&c);
    snprintf(c.mqtt.password, sizeof c.mqtt.password, "broker-pw");
    CHECK(apply("{\"mqtt\":{\"password\":\"\"}}",&c,err,sizeof err)==ESP_OK, "mqtt patch rejected: %s", err);
    CHECK(strcmp(c.mqtt.password,"broker-pw")==0, "empty mqtt password erased the stored one");
    {
        char *masked = app_config_to_json(&c, false);
        CHECK(masked && strstr(masked,"broker-pw")==NULL, "mqtt password leaked to the browser");
        free(masked);
    }

    printf("secret masking\n");
    snprintf(c.net.password, sizeof c.net.password, "new-secret");
    char *pub = app_config_to_json(&c, false);
    char *prv = app_config_to_json(&c, true);
    CHECK(pub && strstr(pub,"new-secret")==NULL, "password leaked to the browser");
    CHECK(pub && strstr(pub,"\"password_set\":true")!=NULL, "password_set flag missing");
    CHECK(prv && strstr(prv,"new-secret")!=NULL, "password missing from the stored copy");

    printf("round trip through json\n");
    app_config_t back; app_config_defaults(&back);
    CHECK(apply(prv,&back,err,sizeof err)==ESP_OK, "round trip rejected: %s", err);
    CHECK(memcmp(&back,&c,sizeof back)==0, "round trip changed the config");
    free(pub); free(prv);

    printf("persistence\n");
    app_config_defaults(&c);
    c.lock.gpio = 4;
    CHECK(app_config_save(&c, err, sizeof err)==ESP_OK, "save rejected: %s", err);
    CHECK(app_config_get()->lock.gpio==4, "running config not updated by save");
    CHECK(app_config_init()==ESP_OK, "reload failed");
    CHECK(app_config_get()->lock.gpio==4, "config did not survive a reload");
    CHECK(app_config_reset()==ESP_OK, "reset failed");
    CHECK(app_config_get()->lock.gpio==2, "reset did not restore defaults");

    printf("hardware capabilities json\n");
    char *caps = app_config_hardware_caps_json();
    CHECK(caps!=NULL, "caps json null");
    cJSON *root = cJSON_Parse(caps);
    CHECK(root!=NULL, "caps json unparseable");
    if (root) {
        cJSON *usable = cJSON_GetObjectItem(root,"usable_pins");
        int n = cJSON_GetArraySize(usable), has18=0, has6=0;
        for(int i=0;i<n;i++){ int v=cJSON_GetArrayItem(usable,i)->valueint; if(v==18)has18=1; if(v==6)has6=1; }
        CHECK(has18, "GPIO18 missing from usable pins");
        CHECK(!has6, "flash pin 6 offered as usable");
        CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(root,"input_only_pins"))==6, "expected 6 input-only pins on esp32");
        cJSON_Delete(root);
    }
    free(caps);

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
