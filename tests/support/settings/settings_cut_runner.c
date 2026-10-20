#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_support.h"

int main(int argc, char **argv)
{
    uint8_t image[256];
    ADC_Calibration_t calibration;
    EEPROM_LoadStatus_t result;
    int saved = -1;
    FILE *file;
    const char *action;
    long cut_after;

    if (argc != 4) {
        return 2;
    }
    action = argv[2];
    cut_after = strtol(argv[3], NULL, 10);
    file = fopen(argv[1], "rb");
    if (file == NULL || fread(image, 1U, sizeof(image), file) != sizeof(image)
        || fgetc(file) != EOF || fclose(file) != 0) {
        return 2;
    }

    settings_fake_reset(image);
    settings_fake_attach_file(argv[1]);
    settings_fake_set_cut_after(cut_after);
    result = EEPROM_SysSetting_Get();
    if (strcmp(action, "save") == 0) {
        Sys_Set_BacklightLevel(70U);
        Sys_Set_MainDisplayMode(1U);
        saved = EEPROM_SysSetting_Save();
    } else if (strcmp(action, "load") != 0) {
        return 2;
    }

    Sys_Get_AdcCalibration(&calibration);
    settings_fake_persist();
    printf("{\"result\":\"%s\",\"settings\":\"%s\",\"calibration_status\":\"%s\","
           "\"brightness\":%u,\"display\":%u,\"saved\":%d,\"written\":%u,\"calibration\":\"",
           settings_status_name(result), settings_status_name(Sys_Get_SettingsLoadStatus()),
           settings_status_name(Sys_Get_CalibrationLoadStatus()),
           (unsigned)Sys_Get_BacklightLevel(), (unsigned)Sys_Get_MainDisplayMode(),
           saved, settings_fake_written);
    const uint8_t *bytes = (const uint8_t *)&calibration;
    for (unsigned i = 0U; i < sizeof(calibration); ++i) {
        printf("%02x", bytes[i]);
    }
    puts("\"}");
    return 0;
}
