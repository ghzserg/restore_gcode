// (C) 2025 ghzserg https://github.com/ghzserg/zmod/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "cJSON.h"

#define BUFFER_SIZE 10240
#define MAX_CMD_LEN BUFFER_SIZE+128

int n=1;
int en=1;
char *str_request=NULL;

int send_command(int sockfd, const char *command);

long find_previous_newline(FILE *f, long end_pos) {
    long pos = end_pos;
    while (pos >= 0) {
        if (fseek(f, pos, SEEK_SET) != 0) {
            return -1;
        }
        int c = fgetc(f);
        if (c == '\n') {
            return pos;
        } else if (c == EOF) {
            return -1;
        }
        pos--;
    }
    return -1;
}

char *find_exclude_start(FILE *f, long end_pos, int sockfd) {
#define SEND_CMD(cmd) do { if (send_command(sockfd, cmd) != 0) break; } while(0)
    long start_pos = -1;
    long current_pos = 0;
    char line_buffer[BUFFER_SIZE];
    char *result_line = NULL;
//    char cmd[MAX_CMD_LEN];

//    SEND_CMD("EXCLUDE_OBJECT_DEFINE RESET=1");
    // Перемещаемся в начало файла
    rewind(f);

    while (fgets(line_buffer, sizeof(line_buffer), f)) {
        current_pos = ftell(f) - strlen(line_buffer); // Начало текущей строки

        // Прекращаем поиск, если вышли за границу end_pos
        if (current_pos > end_pos) break;

        // Проверяем строку на наличие меток
        int has_stop = (strstr(line_buffer, "EXCLUDE_OBJECT_STOP") != NULL);
        int has_start = (strstr(line_buffer, "EXCLUDE_OBJECT_START") != NULL);
//        int has_define = (strstr(line_buffer, "EXCLUDE_OBJECT_DEFINE") != NULL);

//        if (has_define) {
////            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"%s\"", line_buffer);
////            SEND_CMD(cmd);
//            SEND_CMD(line_buffer);
//        }

        if (has_stop) {
            start_pos = -1;
        } else if (has_start) {
            start_pos = current_pos;
        }
    }

    // Возвращаем результат, если нашли стартовую позицию
    if (start_pos != -1) {
        fseek(f, start_pos, SEEK_SET);
        result_line = malloc(BUFFER_SIZE);
        if (fgets(result_line, BUFFER_SIZE, f)) {
            // Обрезаем символ перевода строки
            char *newline = strchr(result_line, '\n');
            if (newline) *newline = '\0';
        } else {
            free(result_line);
            result_line = NULL;
        }
    }

#undef SEND_CMD
    return result_line;
}

void create_json_request(const char *str) {
    // Создаем JSON-запрос
    cJSON *request = cJSON_CreateObject();
    if (!request) {
        return;
    }

    cJSON_AddNumberToObject(request, "id", n++);
    cJSON_AddStringToObject(request, "method", "gcode/script");

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "script", str);

    // Добавление "params" в основной объект
    cJSON_AddItemToObject(request, "params", params);

    str_request = cJSON_PrintUnformatted(request);
    //printf("%s\n", str_request);
    cJSON_Delete(request);
}

int send_command(int sockfd, const char *command) {
    char buffer[BUFFER_SIZE];
    create_json_request(command);

    const char terminator = '\x03';

    if (str_request==NULL)
        return -1;

    if (send(sockfd, str_request, strlen(str_request), 0) == -1 || send(sockfd, &terminator, 1, 0) == -1) {
        perror("Ошибка отправки\n");
        close(sockfd);
        return -1;
    }

    ssize_t bytes_read = read(sockfd, buffer, sizeof(buffer) - 1);
    if (bytes_read < 0) {
        perror("Ошибка: Не удалось прочитать данные из сокета.");
        return -1;
    }

    if (bytes_read == 0) {
        perror("Соединение закрыто сервером.");
        return -1;
    }

    // Поиск символа 0x03 (EOT) в полученном буфере
    for (ssize_t i = 0; i < bytes_read; i++) {
        if (buffer[i] == 0x03) {
            buffer[i] = '\0'; // Заменяем 0x03 на нулевой байт для завершения строки
            break;
        }
    }

    buffer[bytes_read] = '\0'; // Завершаем строку
    //printf("%s\n",buffer);

    return 0;
}

char* read_file(const char *filename) {
    FILE *f = fopen(filename, "rt");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *data = malloc(len + 1);
    fread(data, 1, len, f);
    data[len] = '\0';
    fclose(f);
    return data;
}

int validate_json_structure(cJSON *root) {

    const char *required_fields[] = {
        "position", "absolute_coords", "e_mode", "gcode_offset",
        "pressure_advance", "retract_params", "file_params", "bed_target",
        "extruder_target", "fan_speed", "extrude_factor", "speed_factor",
        "bed_mesh_profile", "excluded_objects",  NULL
    };

    for (int i = 0; required_fields[i]; i++) {
        if (!cJSON_GetObjectItemCaseSensitive(root, required_fields[i])) {
            fprintf(stderr, "Отсуствует обязательное поле: %s\n", required_fields[i]);
            return 0;
        }
    }

    cJSON *item;
    if (!cJSON_IsArray((item = cJSON_GetObjectItem(root, "position"))) || cJSON_GetArraySize(item) != 4) {
        fprintf(stderr, "Invalid position format\n");
        return 0;
    }

    if (!cJSON_IsBool((item = cJSON_GetObjectItem(root, "absolute_coords")))) {
        fprintf(stderr, "Invalid absolute_coords format\n");
        return 0;
    }

    if (!cJSON_IsString((item = cJSON_GetObjectItem(root, "e_mode")))) {
        fprintf(stderr, "Invalid e_mode format\n");
        return 0;
    }

    return 1;
}

int generate_gcode(cJSON *root, int sockfd, const char *prefix) {
#define SEND_CMD(cmd) do { if (send_command(sockfd, cmd) != 0) return -1; } while(0)
    char cmd[MAX_CMD_LEN];

    if (en == 1)
        SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Heating nozzle to 200 to detach part from nozzle\"");
    else
        SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Прогреваем сопло до 200, чтобы деталь отлипла от сопла\"");
    SEND_CMD("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=200");
    SEND_CMD("TEMPERATURE_WAIT SENSOR=extruder MINIMUM=200 MAXIMUM=300");

    if (en == 1)
        SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Moving home\"");
    else
        SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Двигаемся домой\"");
    SEND_CMD("G28");
    SEND_CMD("_GOTO_TRASH");

    SEND_CMD("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=120");

    cJSON *position = cJSON_GetObjectItem(root, "position");
    cJSON *e = cJSON_GetArrayItem(position, 3);
    if (e && e->valuedouble != 0) {
        if (en == 1)
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Restoring extruder status. G92 E%.3f\"", e->valuedouble);
        else
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Восстанавливаю статус экструдера. G92 E%.3f\"", e->valuedouble);
        SEND_CMD(cmd);
        snprintf(cmd, sizeof(cmd), "G92 E%.3f", e->valuedouble);
        SEND_CMD(cmd);
    }

    const char *e_mode = cJSON_GetObjectItem(root, "e_mode")->valuestring;
    if (en == 1) {
        if (strcmp(e_mode, "absolute"))
            SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Setting absolute extruder mode. M82\"");
        else
            SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Setting relative extruder mode. M83\"");
    } else {
        if (strcmp(e_mode, "absolute"))
            SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Устанавливаю абсолютный режим работы экструдера. M82\"");
        else
            SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Устанавливаю относительный режим работы экструдера. M83\"");
    }
    SEND_CMD(strcmp(e_mode, "absolute") ? "M83" : "M82");

    cJSON *offset = cJSON_GetObjectItem(root, "gcode_offset");
    double x = cJSON_GetObjectItem(offset, "x_offset")->valuedouble;
    double y = cJSON_GetObjectItem(offset, "y_offset")->valuedouble;
    double z = cJSON_GetObjectItem(offset, "z_offset")->valuedouble;
    if (x != 0 || y != 0 || z != 0) {
        if (en == 1)
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Setting offset. X=%.3f Y=%.3f Z=%.3f\"", x, y, z);
        else
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Устанавливаю офсет. X=%.3f Y=%.3f Z=%.3f\"", x, y, z);
        SEND_CMD(cmd);
        snprintf(cmd, sizeof(cmd), "SET_GCODE_OFFSET X=%.3f Y=%.3f Z=%.3f FROM=RESTORE", x, y, z);
        SEND_CMD(cmd);
    }

    const char *mesh = cJSON_GetObjectItem(root, "bed_mesh_profile")->valuestring;
    if (strlen(mesh) > 0) {
        if (en == 1)
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Loading bed profile %s\"", mesh);
        else
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Загружаю профиль стола %s\"", mesh);
        SEND_CMD(cmd);
        snprintf(cmd, sizeof(cmd), "_ZRESTORE_MESH_LOAD MESH=%s", mesh);
        SEND_CMD(cmd);
    }


    cJSON *pa = cJSON_GetObjectItem(root, "pressure_advance");
    double adv = cJSON_GetObjectItem(pa, "value")->valuedouble;
    double smooth = cJSON_GetObjectItem(pa, "smooth_time")->valuedouble;
    if (adv > 0) {
        if (en == 1)
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Setting pressure advance %.3f, PA smoothing time %.3f\"", adv, smooth);
        else
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Устанавливаю параметры pressure advance %.3f, время сглаживания PA %.3f\"", adv, smooth);
        SEND_CMD(cmd);
        snprintf(cmd, sizeof(cmd), "SET_PRESSURE_ADVANCE ADVANCE=%.3f ADVANCE_SMOOTHING=%.3f", adv, smooth);
        SEND_CMD(cmd);
    }

    cJSON *retract = cJSON_GetObjectItem(root, "retract_params");
    double r_len = cJSON_GetObjectItem(retract, "retract_length")->valuedouble;
    double r_speed = cJSON_GetObjectItem(retract, "retract_speed")->valuedouble;
    double ur_len = cJSON_GetObjectItem(retract, "unretract_length")->valuedouble;
    double ur_speed = cJSON_GetObjectItem(retract, "unretract_speed")->valuedouble;
    if (r_len > 0) {
        if (en == 1)
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Setting retract parameters (length %.3f, speed %.3f, extra length %.3f, feed speed %.3f)\"",
                r_len, r_speed, ur_len, ur_speed);
        else
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Устанавливаю параметры ретракта (длина %.3f, скорость %.3f, доп. длина подачи %.3f, скорость подачи %.3f)\"",
                r_len, r_speed, ur_len, ur_speed);
        SEND_CMD(cmd);
        snprintf(cmd, sizeof(cmd), "SET_RETRACTION RETRACT_LENGTH=%.3f RETRACT_SPEED=%.3f UNRETRACT_EXTRA_LENGTH=%.3f UNRETRACT_SPEED=%.3f",
                r_len, r_speed, ur_len, ur_speed);
        SEND_CMD(cmd);
    }

    double bed_target = cJSON_GetObjectItem(root, "bed_target")->valuedouble;
    if (bed_target > 0) {
        if (en == 1)
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Heating bed to %.1f degrees\"", bed_target);
        else
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Грею стол до %.1f градусов\"", bed_target);
        SEND_CMD(cmd);
        snprintf(cmd, sizeof(cmd), "M190 S%.1f", bed_target);
        SEND_CMD(cmd);
    }

    double extruder_target = cJSON_GetObjectItem(root, "extruder_target")->valuedouble;
    if (extruder_target > 0) {
        if (en == 1)
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Heating extruder to %.1f degrees\"", extruder_target);
        else
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Грею экструдер до %.1f градусов\"", extruder_target);
        SEND_CMD(cmd);
        snprintf(cmd, sizeof(cmd), "M109 S%.1f", extruder_target);
        SEND_CMD(cmd);
    }

    SEND_CMD("LOAD_CELL_TARE");

    double fan_speed = cJSON_GetObjectItem(root, "fan_speed")->valuedouble;
    if (fan_speed > 0) {
        if (en == 1)
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Setting fan speed to %d\"", (int)(fan_speed * 255));
        else
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Включаю кулер до %d\"", (int)(fan_speed * 255));
        SEND_CMD(cmd);
        snprintf(cmd, sizeof(cmd), "M106 S%d", (int)(fan_speed * 255));
        SEND_CMD(cmd);
    }

    double extrude_factor = cJSON_GetObjectItem(root, "extrude_factor")->valuedouble;
    if (extrude_factor > 0) {
        if (en == 1)
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Setting flow rate to %d\"", (int)(extrude_factor * 100));
        else
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Устанавливаю поток %d\"", (int)(extrude_factor * 100));
        SEND_CMD(cmd);
        snprintf(cmd, sizeof(cmd), "M221 S%d", (int)(extrude_factor * 100));
        SEND_CMD(cmd);
    }


    double speed_factor = cJSON_GetObjectItem(root, "speed_factor")->valuedouble;
    if (speed_factor > 0) {
        if (en == 1)
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Setting speed to %d\"", (int)(speed_factor * 100));
        else
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Устанавливаю скорость %d\"", (int)(speed_factor * 100));
        SEND_CMD(cmd);
        snprintf(cmd, sizeof(cmd), "_SPEED_SET SPEED=%d", (int)(speed_factor * 100));
        SEND_CMD(cmd);
    }

    cJSON *file_params = cJSON_GetObjectItem(root, "file_params");
    const char *fpath = cJSON_GetObjectItem(file_params, "file_path")->valuestring;

    if (strstr(fpath, prefix) == fpath) fpath += strlen(prefix);

    if (strlen(fpath) > 0) {
        if (en == 1)
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Loading file '%s'\"", fpath);
        else
            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Загружаю файл '%s'\"", fpath);
        SEND_CMD(cmd);
        snprintf(cmd, sizeof(cmd), "M23 %s", fpath);
        SEND_CMD(cmd);

        if (en == 1)
            SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Loading exclude objects\"");
        else
            SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Загружаю исключение объектов\"");

        SEND_CMD("EXCLUDE_OBJECT_DEFINE RESET=1");
        snprintf(cmd, sizeof(cmd), "ZEXCLUDE FILENAME=\"%s\"", fpath);
        SEND_CMD(cmd);

        long fpos = (long)cJSON_GetObjectItem(file_params, "file_position")->valuedouble;
        if (fpos > 0) {
            if (en == 1)
                snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Saved file position %ld\"", fpos);
            else
                snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Сохраненная позиция в файле %ld\"", fpos);
            SEND_CMD(cmd);

            FILE *f = fopen(cJSON_GetObjectItem(file_params, "file_path")->valuestring, "rb");
            if (!f) {
                if (en == 1)
                    snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Cannot open file\"");
                else
                    snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Не могу открыть файл\"");
                SEND_CMD(cmd);
                return 1;
            }

            if (en == 1)
                SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Loading object list\"");
            else
                SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Загружаю список объектов\"");
            char *exclude_line = find_exclude_start(f, fpos, sockfd);
            if (exclude_line != NULL) {
                if (en == 1)
                    snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Marking object start\"");
                else
                    snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Помечаю старт объекта\"");
                SEND_CMD(cmd);
                SEND_CMD(exclude_line);
                free(exclude_line);
            }

            // Получаем массив excluded_objects
            cJSON *excluded_objects = cJSON_GetObjectItemCaseSensitive(root, "excluded_objects");
            if (cJSON_IsArray(excluded_objects)) {
                // Перебор элементов массива
                cJSON *current;
                cJSON_ArrayForEach(current, excluded_objects) {
                    if (cJSON_IsString(current)) {
                        if (en==1)
                            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Excluding object %s\"", current->valuestring);
                        else
                            snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Помечаю исключенным объект %s\"", current->valuestring);
                        SEND_CMD(cmd);
                        snprintf(cmd, sizeof(cmd), "EXCLUDE_OBJECT NAME=%s", current->valuestring);
                        SEND_CMD(cmd);
                    }
                }
            }

            fpos = find_previous_newline(f, fpos);
            fclose(f);

            if (en==1)
                snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Moving to calculated file position %ld\"", fpos);
            else
                snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Перехожу к рассчитанной позиции в файле %ld\"", fpos);
            SEND_CMD(cmd);
            snprintf(cmd, sizeof(cmd), "M26 S%ld", fpos);
            SEND_CMD(cmd);
        }

        SEND_CMD("_TEST_MIN_MAX");

        cJSON *x_pos = cJSON_GetArrayItem(position, 0);
        cJSON *y_pos = cJSON_GetArrayItem(position, 1);
        cJSON *z_pos = cJSON_GetArrayItem(position, 2);
        if (x_pos->valuedouble != 0 || y_pos->valuedouble != 0 || z_pos->valuedouble != 0) {
            if (en==1)
                snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Returning nozzle to last position X%.3f Y%.3f Z%.3f F6000\"",
                    x_pos->valuedouble, y_pos->valuedouble, z_pos->valuedouble);
            else
                snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Возвращаю сопло на последнюю позицию X%.3f Y%.3f Z%.3f F6000\"",
                    x_pos->valuedouble, y_pos->valuedouble, z_pos->valuedouble);
            SEND_CMD(cmd);
            SEND_CMD("G90");
            snprintf(cmd, sizeof(cmd), "G1 X%.3f F6000", x_pos->valuedouble);
            SEND_CMD(cmd);
            SEND_CMD("M400");
            snprintf(cmd, sizeof(cmd), "G1 Y%.3f F6000", y_pos->valuedouble);
            SEND_CMD(cmd);
            SEND_CMD("M400");
            snprintf(cmd, sizeof(cmd), "G1 Z%.3f F6000", z_pos->valuedouble);
            SEND_CMD(cmd);
            SEND_CMD("M400");
        }

        if (en==1)
            SEND_CMD(cJSON_GetObjectItem(root, "absolute_coords")->valueint ? "RESPOND PREFIX=\"//\" MSG=\"Setting absolute positioning mode\"" : "RESPOND PREFIX=\"//\" MSG=\"Setting relative positioning mode\"");
        else
            SEND_CMD(cJSON_GetObjectItem(root, "absolute_coords")->valueint ? "RESPOND PREFIX=\"//\" MSG=\"Устанавливаю абсолютный режим смещения\"" : "RESPOND PREFIX=\"//\" MSG=\"Устанавливаю относительный режим смещения\"");
        SEND_CMD(cJSON_GetObjectItem(root, "absolute_coords")->valueint ? "G90" : "G91");

        if (en==1)
            SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Enable IFS\"");
        else
            SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Включаю IFS\"");

        SEND_CMD("_PREPARE_RESTORE");
        SEND_CMD("ZCONTROL_ABORT");
        SEND_CMD("ZCONTROL_ON");

        // Ищем и выполняем SET_PRINT_STATS_INFO
        FILE *print_file = fopen(fpath, "r");
        if (!print_file) {
            char full_path[BUFFER_SIZE];
            snprintf(full_path, sizeof(full_path), "%s/%s", prefix, fpath);
            print_file = fopen(full_path, "r");
        }

        if (print_file) {
            char line[BUFFER_SIZE];
            while (fgets(line, sizeof(line), print_file)) {
                // Проверяем, начинается ли строка с "SET_PRINT_STATS_INFO"
                if (strncmp(line, "SET_PRINT_STATS_INFO", 20) == 0) {
                    // Удаляем символ новой строки, если он есть
                    char *newline = strchr(line, '\n');
                    if (newline) *newline = '\0';

                    // Отправляем команду через Klipper
                    if (en == 1)
                        snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Executing SET_PRINT_STATS_INFO command\"");
                    else
                        snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Выполняю команду SET_PRINT_STATS_INFO\"");
                    SEND_CMD(cmd);
                    SEND_CMD(line);
                    break;
                }
            }
            fclose(print_file);
        } else {
            if (en == 1)
                snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Could not open print file to find SET_PRINT_STATS_INFO\"");
            else
                snprintf(cmd, sizeof(cmd), "RESPOND PREFIX=\"//\" MSG=\"Не удалось открыть файл печати для поиска SET_PRINT_STATS_INFO\"");
            SEND_CMD(cmd);
        }

        SEND_CMD("_PREPARE_RESTORE");

        if (en==1)
            SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Starting print\"");
        else
            SEND_CMD("RESPOND PREFIX=\"//\" MSG=\"Запускаю печать\"");
        SEND_CMD("M24");

        SEND_CMD("_AFTER_RESTORE");
    }
#undef SEND_CMD
    return 0;
}

int main(int argc, char *argv[]) {
    const char *filename = (argc > 1) ? argv[1] : "/opt/config/mod_data/klipper_data.json";
    const char *socker_name = (argc > 2) ? argv[2] : "/tmp/uds";
    const char *dir_name = (argc > 3) ? argv[3] : "/data";
    const char *lang = (argc > 4) ? argv[4] : "en";
    if (strncmp(lang, "ru", 2) == 0) {
        en = 0;
    }

    char *json_data = read_file(filename);
    if (!json_data) {
        if (en==1)
            fprintf(stderr, "Error reading file %s\n", filename);
        else
            fprintf(stderr, "Ошибка чтения файла %s\n", filename);
        return 1;
    }

    cJSON *json = cJSON_Parse(json_data);
    free(json_data);
    if (!json || !validate_json_structure(json)) {
        if (en==1)
            fprintf(stderr, "JSON error\n");
        else
            fprintf(stderr, "Ошибка json\n");
        cJSON_Delete(json);
        return 1;
    }

    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd == -1) {
        if (en==1)
            perror("Can not make socket");
        else
            perror("Не могу создать сокет");
        cJSON_Delete(json);
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socker_name, sizeof(addr.sun_path) - 1);

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        if (en==1)
            perror("Failed to connect to Klipper");
        else
            perror("Соединение с клиппер неуспешно");
        close(sockfd);
        cJSON_Delete(json);
        return 1;
    }

    daemon(0, 0);
    int result = generate_gcode(json, sockfd, dir_name);
    close(sockfd);
    cJSON_Delete(json);

    if (result != 0) {
        if (en==1)
            fprintf(stderr, "Errors sending data to Klipper\n");
        else
            fprintf(stderr, "Ошибки при отправке данных в клиппер\n");
        return 1;
    }

    if (en==1)
        printf("Recovery data sent successfully\n");
    else
        printf("Данные восстановления успешно отправлены\n");
    return 0;
}
