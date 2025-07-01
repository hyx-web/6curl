#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>

#define WIDTH 600
#define HEIGHT 700
#define THREAD_COUNT 16 // 定义线程数

typedef struct {
    Display *display;
    Window window;
    GC gc;
    int width;
    int height;
    int input_x, input_y, input_w, input_h;
    char input_text[1024];
    int cursor_pos;
    int input_active;
    int button_x, button_y, button_w, button_h;
} App;

// 线程下载状态结构
typedef struct {
    const char *url;
    const char *filename;
    long start_byte;
    long end_byte;
    int thread_id;
    char status[256];
    long http_status;
    CURLcode curl_res;
    double download_speed;
} ThreadData;

void draw_button(App *app, int x, int y, int w, int h, const char *label) {
    XDrawRectangle(app->display, app->window, app->gc, x, y, w, h);
    XDrawString(app->display, app->window, app->gc, x + 10, y + h / 2, label, strlen(label));
}

int is_point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return (px >= rx) && (px <= rx + rw) && (py >= ry) && (py <= ry + rh);
}

size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    FILE *fp = (FILE *)userdata;
    return fwrite(ptr, size, nmemb, fp);
}

// 进度回调函数用于计算下载速度
static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, 
                            curl_off_t ultotal, curl_off_t ulnow) {
    ThreadData *data = (ThreadData *)clientp;
    static time_t last_time = 0;
    static curl_off_t last_dlnow = 0;
    time_t now = time(NULL);
    
    if (now > last_time) {
        if (last_dlnow > 0) {
            curl_off_t bytes_per_sec = (dlnow - last_dlnow) / (now - last_time);
            data->download_speed = bytes_per_sec;
        }
        last_dlnow = dlnow;
        last_time = now;
    }
    return 0;
}

char *extract_filename(const char *url) {
    const char *last_slash = strrchr(url, '/');
    if (last_slash == NULL) {
        return strdup(url);
    }
    if (*(last_slash + 1) == '\0') {
        return NULL;
    }
    const char *filename_start = last_slash + 1;
    const char *question_mark = strchr(filename_start, '?');
    if (question_mark != NULL) {
        size_t filename_len = question_mark - filename_start;
        char *filename = malloc(filename_len + 1);
        strncpy(filename, filename_start, filename_len);
        filename[filename_len] = '\0';
        return filename;
    } else {
        return strdup(filename_start);
    }
}

// 获取文件大小
long get_file_size(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); // HEAD请求
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "6curl/1.0");
    
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return -1;
    }
    
    double file_size = 0;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &file_size);
    curl_easy_cleanup(curl);
    
    return (long)file_size;
}

// 单个线程的下载函数
void *download_thread(void *arg) {
    ThreadData *data = (ThreadData *)arg;
    
    // 创建临时文件名
    char temp_filename[PATH_MAX];
    snprintf(temp_filename, sizeof(temp_filename), "%s.part%d", data->filename, data->thread_id);
    
    FILE *fp = fopen(temp_filename, "wb");
    if (!fp) {
        snprintf(data->status, sizeof(data->status), "无法创建临时文件");
        return NULL;
    }
    
    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        snprintf(data->status, sizeof(data->status), "curl初始化失败");
        return NULL;
    }
    
    // 设置下载范围
    char range_header[50];
    snprintf(range_header, sizeof(range_header), "%ld-%ld", data->start_byte, data->end_byte);
    
    curl_easy_setopt(curl, CURLOPT_URL, data->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_RANGE, range_header);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "6curl/1.0");
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, data);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    
    data->curl_res = curl_easy_perform(curl);
    
    // 获取HTTP状态码
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &data->http_status);
    
    fclose(fp);
    curl_easy_cleanup(curl);
    
    if (data->curl_res == CURLE_OK && 
        (data->http_status == 200 || data->http_status == 206)) {
        snprintf(data->status, sizeof(data->status), "完成 (%.2f KB/s)", 
                 data->download_speed / 1024.0);
    } else {
        snprintf(data->status, sizeof(data->status), "失败 (%s)", 
                 curl_easy_strerror(data->curl_res));
    }
    
    return NULL;
}

// 合并下载的部分文件
int merge_files(const char *filename, int thread_count) {
    FILE *output = fopen(filename, "wb");
    if (!output) return 0;
    
    int success = 1;
    
    for (int i = 0; i < thread_count; i++) {
        char part_filename[PATH_MAX];
        snprintf(part_filename, sizeof(part_filename), "%s.part%d", filename, i);
        
        FILE *input = fopen(part_filename, "rb");
        if (!input) {
            success = 0;
            continue;
        }
        
        char buffer[8192];
        size_t nread;
        while ((nread = fread(buffer, 1, sizeof(buffer), input)) > 0) {
            fwrite(buffer, 1, nread, output);
        }
        
        fclose(input);
        remove(part_filename); // 删除临时文件
    }
    
    fclose(output);
    return success;
}

// 多线程下载主函数
void multithread_download(const char *url, const char *filename) {
    printf("开始下载: %s\n", url);
    printf("保存到: %s\n", filename);
    
    // 获取文件大小
    long file_size = get_file_size(url);
    if (file_size <= 0) {
        printf("无法获取文件大小，使用单线程下载\n");
        // 使用单线程下载
        FILE *fp = fopen(filename, "wb");
        if (!fp) {
            printf("无法创建文件: %s\n", filename);
            return;
        }
        
        CURL *curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "6curl/1.0");
            
            CURLcode res = curl_easy_perform(curl);
            long http_code;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            
            if (res != CURLE_OK || (http_code != 200 && http_code != 201 && http_code != 202)) {
                printf("下载失败: %s (HTTP %ld)\n清理中...\n", 
                       curl_easy_strerror(res), http_code);
                fclose(fp);
                remove(filename);
            } else {
                printf("下载成功\n");
                fclose(fp);
            }
            curl_easy_cleanup(curl);
        } else {
            fclose(fp);
        }
        return;
    }
    
    printf("文件大小: %.2f MB\n", (double)file_size / (1024 * 1024));
    
    // 计算每个线程的大小
    long chunk_size = file_size / THREAD_COUNT;
    pthread_t threads[THREAD_COUNT];
    ThreadData thread_data[THREAD_COUNT];
    
    // 创建线程
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_data[i].url = url;
        thread_data[i].filename = filename;
        thread_data[i].thread_id = i;
        thread_data[i].start_byte = i * chunk_size;
        thread_data[i].end_byte = (i == THREAD_COUNT - 1) ? file_size - 1 : (i + 1) * chunk_size - 1;
        thread_data[i].download_speed = 0;
        snprintf(thread_data[i].status, sizeof(thread_data[i].status), "准备中...");
        
        if (pthread_create(&threads[i], NULL, download_thread, &thread_data[i]) != 0) {
            perror("无法创建线程");
            snprintf(thread_data[i].status, sizeof(thread_data[i].status), "线程创建失败");
        }
        else {
            snprintf(thread_data[i].status, sizeof(thread_data[i].status), "下载中...");
        }
    }
    
    // 等待所有线程完成
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
        printf("线程 %d: %s\n", i, thread_data[i].status);
    }
    
    // 合并文件
    if (merge_files(filename, THREAD_COUNT)) {
        printf("下载完成并合并成功\n");
    } else {
        printf("文件合并失败\n");
        remove(filename);
    }
}

void start_download(const char *url) {
    char *filename = extract_filename(url);
    if (filename == NULL) {
        printf("URL 没有文件名\n");
        return;
    }

    char filepath[PATH_MAX];
    snprintf(filepath, sizeof(filepath), "/home/%s/%s", getlogin(), filename);
    printf("输出文件: %s\n", filepath);

    // 使用多线程下载
    multithread_download(url, filepath);
    
    free(filename);
}

void redraw(App *app) {
    XDrawRectangle(app->display, app->window, app->gc, app->input_x, app->input_y, app->input_w, app->input_h);
    XDrawString(app->display, app->window, app->gc, app->input_x + 5, app->input_y + app->input_h / 2, app->input_text, strlen(app->input_text));
    if (app->input_active) {
        XGCValues gc_values;
        if (XGetGCValues(app->display, app->gc, GCFont, &gc_values) != 0) {
            XFontStruct *font_struct = XQueryFont(app->display, gc_values.font);
            if (font_struct) {
                int text_width = XTextWidth(font_struct, app->input_text, app->cursor_pos);
                int cursor_x = app->input_x + 5 + text_width;
                XDrawLine(app->display, app->window, app->gc, cursor_x, app->input_y + 5, cursor_x, app->input_y + app->input_h - 5);
                XFreeFont(app->display, font_struct);
            }
        }
    }
    draw_button(app, app->button_x, app->button_y, app->button_w, app->button_h, "Download");
    XFlush(app->display);
}

int main() {
    App app;
    app.display = XOpenDisplay(NULL);
    if (!app.display) {
        fprintf(stderr, "Failed to open X display\n");
        return 1;
    }

    int screen = DefaultScreen(app.display);
    app.window = XCreateSimpleWindow(app.display, RootWindow(app.display, screen),
                                     0, 0, WIDTH, HEIGHT, 1,
                                     BlackPixel(app.display, screen),
                                     WhitePixel(app.display, screen));
    XStoreName(app.display, app.window, "6curl");

    XSelectInput(app.display, app.window, ExposureMask | ButtonPressMask | KeyPressMask | StructureNotifyMask);
    app.gc = XCreateGC(app.display, app.window, 0, NULL);

    XFontStruct *font = XLoadQueryFont(app.display, "fixed");
    if (font) {
        XSetFont(app.display, app.gc, font->fid);
    } else {
        fprintf(stderr, "无法加载字体\n");
        XFreeGC(app.display, app.gc);
        XDestroyWindow(app.display, app.window);
        XCloseDisplay(app.display);
        return 1;
    }

    XMapWindow(app.display, app.window);
    app.input_x = 50;
    app.input_y = 30;
    app.input_w = 500;
    app.input_h = 30;
    memset(app.input_text, 0, sizeof(app.input_text));
    app.cursor_pos = 0;
    app.input_active = 0;
    app.button_x = 50;
    app.button_y = 80;
    app.button_w = 100;
    app.button_h = 40;

    int running = 1;
    while (running) {
        XEvent event;
        XNextEvent(app.display, &event);
        if (event.type == Expose) {
            redraw(&app);
        } else if (event.type == ButtonPress) {
            int px = event.xbutton.x;
            int py = event.xbutton.y;
            if (is_point_in_rect(px, py, app.input_x, app.input_y, app.input_w, app.input_h)) {
                app.input_active = 1;
            } else if (is_point_in_rect(px, py, app.button_x, app.button_y, app.button_w, app.button_h)) {
                if (strlen(app.input_text) > 0) {
                    printf("下载开始...\n");
                    start_download(app.input_text);
                    printf("下载完成\n");
                } else {
                    printf("请输入 URL\n");
                }
            } else {
                app.input_active = 0;
            }
            redraw(&app);
        } else if (event.type == KeyPress) {
            if (app.input_active) {
                KeySym key;
                char buf[10];
                int len = XLookupString(&event.xkey, buf, sizeof(buf), &key, NULL);

                if ((event.xkey.state & ControlMask) && key == XK_v) {
                    Atom selection = XInternAtom(app.display, "CLIPBOARD", False);
                    XConvertSelection(app.display, selection, XA_STRING, XA_STRING, app.window, CurrentTime);
                    continue;
                }

                if (len > 0 && key >= XK_space && key <= XK_asciitilde) {
                    if (strlen(app.input_text) < sizeof(app.input_text) - 1) {
                        memmove(&app.input_text[app.cursor_pos + 1], &app.input_text[app.cursor_pos], strlen(&app.input_text[app.cursor_pos]) + 1);
                        app.input_text[app.cursor_pos] = buf[0];
                        app.cursor_pos++;
                    }
                } else if (key == XK_BackSpace && app.cursor_pos > 0) {
                    memmove(&app.input_text[app.cursor_pos - 1], &app.input_text[app.cursor_pos], strlen(&app.input_text[app.cursor_pos]) + 1);
                    app.cursor_pos--;
                } else if (key == XK_Delete && app.cursor_pos < strlen(app.input_text)) {
                    memmove(&app.input_text[app.cursor_pos], &app.input_text[app.cursor_pos + 1], strlen(&app.input_text[app.cursor_pos + 1]) + 1);
                } else if (key == XK_Left && app.cursor_pos > 0) {
                    app.cursor_pos--;
                } else if (key == XK_Right && app.cursor_pos < strlen(app.input_text)) {
                    app.cursor_pos++;
                }
                XClearWindow(app.display, app.window);
                redraw(&app);
            }
        } else if (event.type == ConfigureNotify) {
            XConfigureEvent xce = event.xconfigure;
            app.width = xce.width;
            app.height = xce.height;
            app.input_w = app.width - 100;
            app.button_x = (app.width - app.button_w) / 2;
            redraw(&app);
        } else if (event.type == SelectionNotify) {
            if (event.xselection.property == XA_STRING) {
                Atom type;
                int format;
                unsigned long nitems, bytes_after;
                char *data;
                XGetWindowProperty(app.display, app.window, XA_STRING, 0, 0, False, AnyPropertyType, &type, &format, &nitems, &bytes_after, (unsigned char **)&data);
                if (bytes_after > 0) {
                    XGetWindowProperty(app.display, app.window, XA_STRING, 0, bytes_after, False, AnyPropertyType, &type, &format, &nitems, &bytes_after, (unsigned char **)&data);
                    if (data) {
                        int paste_len = strlen(data);
                        if (paste_len > 0) {
                            int space_left = sizeof(app.input_text) - strlen(app.input_text) - 1;
                            if (paste_len <= space_left) {
                                memmove(&app.input_text[app.cursor_pos + paste_len], &app.input_text[app.cursor_pos], strlen(&app.input_text[app.cursor_pos]) + 1);
                                memcpy(&app.input_text[app.cursor_pos], data, paste_len);
                                app.cursor_pos += paste_len;
                            }
                        }
                        XFree(data);
                    }
                }
                XDeleteProperty(app.display, app.window, XA_STRING);
                XClearWindow(app.display, app.window);
                redraw(&app);
            }
        }
    }

    if (font) {
        XFreeFont(app.display, font);
    }
    XFreeGC(app.display, app.gc);
    XDestroyWindow(app.display, app.window);
    XCloseDisplay(app.display);
    return 0;
}
