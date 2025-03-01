#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bt.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>

#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>

#include <gui/elements.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>

#include "uart.h"

#include <momentum/momentum.h> //支持MNTM > Protocols > GPIO Pins > ESP32/8266 UART:


//定义枚举类型subMenu
typedef enum {
    subMenuSetup,
    subMenuScan,
    subMenuWeapon,
    subMenuConsole,
    subMenuAbout
} subMenu;

//定义枚举类型ViewList
typedef enum {
    subMenuViewMain,
    subMenuViewSetup,
    subMenuViewScan,
    subMenuViewWeapon,
    subMenuViewConsole,
    subMenuViewAbout
} ViewList;

//定义枚举类型Weapon_List
typedef enum {
    Weapon_OKLOK_UNLOCK,
    Weapon_Aerlang,
    Weapon_COUNT
} Weapon_List;

//定义结构体类型bleScanCtx
//定义时成员结尾加*例如ViewDispatcher*意味着定义了一个指针型的变量或类型
//变量或类型前面加*例如*view_list[6]意味着该变量或类型是指针型的
typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu; //定义Submenu*指针的初始值为上面21行定义的枚举类型subMenu
    void* view_list[6]; //定义6个字符长度的指针变量*view_list
    NotificationApp* notification;
    VariableItemList* variable_item_list;
    uart_app* uapp;
    View* text_box_view; //兼容性
    View* scan_view; //兼容性
    View* weapon_view; //兼容性

    Widget* widget; /////
} bleScanCtx;

//定义结构体类型SkeletonWeaponModel
typedef struct {
    signed int state_index;
    bool is_start;
} SkeletonWeaponModel;

//定义结构体类型SkeletonScanModel
typedef struct {
    bleScanCtx* widget;
    FuriString* s;
} SkeletonScanModel;

NotificationMessage blink_message = {

    //“.”代表结构体成员的访问操作符
    .type = NotificationMessageTypeLedBlinkStart,
    .data.led_blink.color = LightBlue | LightGreen,
    .data.led_blink.on_time = 10,
    .data.led_blink.period = 100,
};

const NotificationSequence blink_sequence = {
    &blink_message,
    &message_do_not_reset,
    NULL,
};

typedef enum {
    WorkerEvtStop = (1 << 0),
    WorkerEvtRxDone = (1 << 1),
} WorkerEvtFlags;

#define WORKER_ALL_RX_EVENTS (WorkerEvtStop | WorkerEvtRxDone) //定义

//定义UART通道
//#define UART_CH 1 //定义
#define UART_CH \
    (momentum_settings.uart_esp_channel) //支持MNTM > Protocols > GPIO Pins > ESP32/8266 UART:

#define UART_TERMINAL_TEXT_BOX_STORE_SIZE 4096 //定义UART终端文本框存储大小

#define TAG "ble_killer" //定义日志标题

#define MAX_DEVICE_SCAN 5 //定义蓝牙扫描结果最大显示数量

bool scanEndFlag = false; //定义

char* device[MAX_DEVICE_SCAN]; //定义

// 封装安全的内存分配函数
void* safe_malloc(size_t size) {
    void* ptr = malloc(size);
    if(ptr == NULL) {
        FURI_LOG_E(TAG, "Memory allocation failed");
    }
    return ptr;
}

// 开始闪烁LED的函数
static void start_blink(bleScanCtx* ctx) {
    // uint16_t period = delays[state->delay];
    // if(period <= 100) period += 30;
    blink_message.data.led_blink.period = 30;
    notification_message(ctx->notification, &blink_sequence);
}
// 停止闪烁LED的函数
static void stop_blink(bleScanCtx* ctx) {
    notification_message(ctx->notification, &sequence_blink_stop);
}

//自定义的字符串分割函数
char* flipbip_strtok_r(char* s, const char* delim, char** last) {
    char* spanp;
    int c, sc;
    char* tok;
    if(s == NULL && (s = *last) == NULL) return NULL;
    // Skip (span) leading delimiters (s += strspn(s, delim), sort of).
cont:
    c = *s++;
    for(spanp = (char*)delim; (sc = *spanp++) != 0;) {
        if(c == sc) goto cont;
    }
    if(c == 0) { // no non-delimiter characters
        *last = NULL;
        return NULL;
    }
    tok = s - 1;
    // Scan token (scan for delimiters: s += strcspn(s, delim), sort of).
    // Note that delim must have one NUL; we stop if we see that, too.
    for(;;) {
        c = *s++;
        spanp = (char*)delim;
        do {
            if((sc = *spanp++) == c) {
                if(c == 0)
                    s = NULL;
                else
                    s[-1] = 0;
                *last = s;
                return tok;
            }
        } while(sc != 0);
    }
    // Unreachable code, but added for completeness.
    return NULL;
}

char* flipbip_strtok(char* s, const char* delim) {
    static char* last;
    return flipbip_strtok_r(s, delim, &last);
}

static void scan_console_recv(void* ctx) {
    uart_app* uapp = ((bleScanCtx*)ctx)->uapp;
    if(scanEndFlag == true) {
        if(uapp->scan_timer) {
            furi_timer_free(uapp->scan_timer);
            uapp->scan_timer = NULL;
        }
        return;
    }

    TextBox* text_box = uapp->text_box;

    uart_terminal_uart_set_handle_rx_data_cb(
        ((uart_app*)uapp),
        uart_terminal_console_output_handle_rx_data_cb); // setup callback for rx thread

    text_box_set_text(text_box, furi_string_get_cstr(((uart_app*)uapp)->text_box_store));

    char recv_buf[512];
    // // memset(recv_buf, '\0', sizeof(recv_buf));
    memcpy(recv_buf, furi_string_get_cstr(((uart_app*)uapp)->text_box_store), sizeof(recv_buf));

    if(strstr(furi_string_get_cstr(((uart_app*)uapp)->text_box_store), ";")) {
        scanEndFlag = true;

        char* token = flipbip_strtok(recv_buf, ",");
        // UNUSED(token);
        int i = 0;
        while(token != NULL && i < MAX_DEVICE_SCAN) {
            device[i] = (char*)safe_malloc(64);
            if(device[i] == NULL) {
                FURI_LOG_E(TAG, "Failed to allocate memory for device string");
                break;
            }
            memcpy(device[i], token, strlen(token));
            i++;
            token = flipbip_strtok(NULL, ",");
        }

        // furi_string_printf(uapp->text_box_store, "recv buf:%s,;", uapp->rx_buf);
        // TextBox* text_box = uapp->text_box;
        // text_box_set_text(text_box, furi_string_get_cstr(uapp->text_box_store));

        stop_blink((bleScanCtx*)ctx);
        view_dispatcher_switch_to_view(((bleScanCtx*)ctx)->view_dispatcher, subMenuViewScan);
    }

    // furi_string_printf(uapp->text_box_store, "recv buf:%s,;", uapp->rx_buf);

    // TextBox* text_box = uapp->text_box;
    // text_box_set_text(text_box, furi_string_get_cstr(uapp->text_box_store));
}

void uart_send_cmd(uart_app* uart, uint8_t* data, size_t len) {
    // furi_hal_uart_tx(uart_ch, data, len);
    furi_hal_serial_tx(uart->serial_handle, data, len);
}

//功能菜单
void skeleton_submenu_callback(void* context, uint32_t index) {
    bleScanCtx* ctx = (bleScanCtx*)context;

    if(index == subMenuScan) {
        view_dispatcher_switch_to_view(ctx->view_dispatcher, subMenuViewScan);
    } else if(index == subMenuWeapon) {
        view_dispatcher_switch_to_view(ctx->view_dispatcher, subMenuViewWeapon);
    } else if(index == subMenuConsole) {
        view_dispatcher_switch_to_view(ctx->view_dispatcher, subMenuViewConsole);
    } else if(index == subMenuAbout) {
        view_dispatcher_switch_to_view(ctx->view_dispatcher, subMenuViewAbout);
    } else if(index == subMenuSetup) {
        view_dispatcher_switch_to_view(ctx->view_dispatcher, subMenuViewSetup);
    }
}

//蓝牙设备扫秒页面
static void view_scan_draw_callback(Canvas* canvas, void* context) {
    // SkeletonScanModel *model = (SkeletonScanModel *)context;

    UNUSED(context);
    if(!scanEndFlag) {
        canvas_set_font(canvas, FontPrimary);
        //canvas_draw_str(canvas, 30, 30, "Press 'OK' to scan.");
        canvas_draw_str(canvas, 15, 34, "点击<确认键>开始扫描");
        // start_blink(_ctx);
        // notification_message(_ctx->notification, &sequence_notification);

    } else {
        // char buf[20] = "Aerlang";
        // FuriString* xstr = furi_string_alloc();
        // furi_string_printf(xstr, "result: %s\n", buf);
        //canvas_set_font(canvas, FontPrimary);
        // canvas_draw_str(canvas, 20, 15, "Result:");
        //canvas_draw_str_aligned(canvas, 0, 12, AlignLeft, AlignBottom, "Result:");
        //canvas_draw_str_aligned(canvas, 0, 12, AlignLeft, AlignBottom, "扫描结果:");

        FuriString* xstr = furi_string_alloc();
        int i;
        for(i = 0; i < MAX_DEVICE_SCAN; i++) {
            if(device[i] != NULL) {
                furi_string_printf(xstr, "%s", device[i]);
                canvas_set_font(canvas, FontSecondary);
                //canvas_set_font(canvas, FontBatteryPercent);
                //canvas_draw_str(canvas, 5, 10 + i * 10, furi_string_get_cstr(xstr));
                canvas_draw_str(canvas, 0, 0 + i * 10, furi_string_get_cstr(xstr));
            }
        }

        // furi_string_printf(xstr, "%s", "notification_message(_ctx->notification, &sequence_blink_stop);\ncanvas_draw_str(canvas, 5, 10+i*10, furi_string_get_cstr(xstr));");
        // elements_scrollable_text_line(canvas, 40, 50, 128, xstr, 0, true );

        // canvas_draw_str(canvas, 50, 50, furi_string_get_cstr(model->s));

        // widget_add_string_element(model->widget->view_list[subMenuViewAbout],10,20, AlignLeft, AlignTop, FontPrimary,furi_string_get_cstr(model->s));
        // widget_add_string_multiline_element(model->widget->view_list[subMenuViewAbout],10,20, AlignLeft, AlignTop, FontPrimary,furi_string_get_cstr(model->s));

        // Bus Fault
        // widget_add_text_scroll_element(model->widget->view_list[subMenuViewScan],0,0,128, 64,"This is a bluetooth ble scanner and controller tool.\n---\nScan any low energy ble device around.");

        furi_string_free(xstr); // 释放字符串
    }
    // elements_button_center(canvas, scanEndFlag ? "Stop":"Start");
}

//攻击页面
static void view_weapon_draw_callback(Canvas* canvas, void* context) {
    // UNUSED(context);
    // FURI_LOG_I(TAG, "view_weapon_draw_callback context null.");
    SkeletonWeaponModel* model = (SkeletonWeaponModel*)context;

    if(model == NULL) {
        FURI_LOG_I(TAG, "view_weapon_draw_callback context null.");
        return;
    }

    FURI_LOG_I(TAG, "View index: %d", model->state_index);

    switch(model->state_index) {
    case Weapon_OKLOK_UNLOCK:
        canvas_set_font(canvas, FontPrimary);

        //canvas_draw_str_aligned(canvas, 124, 12, AlignRight, AlignBottom, "OKLOK unlock");
        canvas_draw_str_aligned(canvas, 124, 12, AlignRight, AlignBottom, "蓝牙指纹锁");
        /*
        elements_text_box(
            canvas,
            4, 30, 80, 48,
            AlignLeft,
            AlignTop,
            "Press \e#OK\e# button\n"
            "To \e#attack\e#",
            false);
        elements_button_center(canvas, model->is_start ? "Stop" : "Start");
        elements_button_right(canvas, "Next");
        break;
        */

        canvas_draw_str_aligned(canvas, 4, 45, AlignLeft, AlignBottom, "点击<确认键>开始攻击");
        elements_button_center(canvas, model->is_start ? "停止" : "开始");
        elements_button_right(canvas, "下个");
        break;

    case Weapon_Aerlang:
        canvas_set_font(canvas, FontPrimary);
        //canvas_draw_str_aligned(canvas, 124, 12, AlignRight, AlignBottom, "Aerlang controller");
        canvas_draw_str_aligned(canvas, 124, 12, AlignRight, AlignBottom, "平衡车控制器");
        /*
        elements_text_box(
            canvas,
            4, 30, 80, 48,
            AlignLeft,
            AlignTop,
            "Press \e#OK\e# button\n"
            "To \e#attack\e#",
            false);
        elements_button_center(canvas, model->is_start ? "Stop" : "Start");
        elements_button_left(canvas, "Pre");
        break;
        */
        canvas_draw_str_aligned(canvas, 4, 45, AlignLeft, AlignBottom, "点击<确认键>开始攻击");
        elements_button_center(canvas, model->is_start ? "停止" : "开始");
        elements_button_left(canvas, "上个");
        break;

    default:
        break;
    }
}

static bool view_weapon_input_callback(InputEvent* input_event, void* context) {
    bleScanCtx* _ctx = (bleScanCtx*)context;
    // FURI_LOG_I(TAG, "Weapon_count: %d\n", Weapon_COUNT);
    if(input_event->type == InputTypeShort) {
        if(input_event->key == InputKeyRight) {
            with_view_model(
                _ctx->view_list[subMenuViewWeapon],
                SkeletonWeaponModel * model,
                {
                    if(model->state_index < Weapon_COUNT - 1) {
                        model->state_index++;
                    }
                },
                true);
            return true;
        } else if(input_event->key == InputKeyLeft) {
            with_view_model(
                _ctx->view_list[subMenuViewWeapon],
                SkeletonWeaponModel * model,
                {
                    if(model->state_index > 0) {
                        model->state_index--;
                    }
                },
                true);
            return true;
        } else if(input_event->key == InputKeyOk) {
            SkeletonWeaponModel* model = view_get_model(_ctx->view_list[subMenuViewWeapon]);
            if(!model->is_start) {
                view_dispatcher_send_custom_event(_ctx->view_dispatcher, model->state_index);
                start_blink(_ctx);
            } else {
                stop_blink(_ctx);
            }
            model->is_start = !model->is_start;
            // 更新下方按钮状态
            view_dispatcher_switch_to_view(_ctx->view_dispatcher, subMenuViewWeapon);

        } else if(input_event->key == InputKeyBack) {
            stop_blink(_ctx);
        }
    }

    return false;
}

static bool weapon_custom_event_callback(uint32_t event, void* context) {
    bleScanCtx* ctx = (bleScanCtx*)context;
    char label[20] = "weapon";

    FuriString* cmd_buf = furi_string_alloc();

    if(ctx->uapp->uart_is_init == 0) {
        // view_dispatcher_send_custom_event(_ctx->view_dispatcher, 2);
        uart_init(ctx->uapp, ctx->uapp->BAUDRATE, UART_CH);
        ctx->uapp->uart_is_init = 1;
    }

    switch(event) {
    case Weapon_OKLOK_UNLOCK:
        FURI_LOG_I(TAG, "Weapon custom event: Weapon_OKLOK_UNLOCK");
        // sprintf(cmd_buf, "%s,%d", label, (int)event);
        furi_string_cat_printf(cmd_buf, "%s,%d", label, (int)event);
        uart_send_cmd(
            ctx->uapp, (uint8_t*)furi_string_get_cstr(cmd_buf), furi_string_size(cmd_buf));
        // return true;
        break;
    case Weapon_Aerlang:
        FURI_LOG_I(TAG, "Weapon custom event: Weapon_Aerlang");
        // sprintf(cmd_buf, "%s,%d", label, (int)event);
        furi_string_cat_printf(cmd_buf, "%s,%d", label, (int)event);
        uart_send_cmd(
            ctx->uapp, (uint8_t*)furi_string_get_cstr(cmd_buf), furi_string_size(cmd_buf));
        // return true;
        break;
    default:
        return false;
    }
    furi_string_free(cmd_buf);
    return false;
}

static bool setup_custom_event_callback(uint32_t event, void* context) {
    bleScanCtx* ctx = (bleScanCtx*)context;
    UNUSED(ctx);
    switch(event) {
    case 0:
        return true;
    default:
        return false;
    }
    return false;
}

// 自定义event的处理
static bool scan_custom_event_callback(uint32_t event, void* context) {
    bleScanCtx* ctx = (bleScanCtx*)context;
    uart_app* uapp = ctx->uapp;

    FURI_LOG_I(TAG, "scan_custom_event_callback event: %ld", event);
    float frequency;
    switch(event) {
    case TEST_BTN:
        if(furi_hal_speaker_acquire(500)) {
            frequency = 200;
            furi_hal_speaker_start(frequency, 1.0);
            furi_delay_ms(100);
            furi_hal_speaker_stop();
            furi_hal_speaker_release();
        }
        // x = 1;

        return true;
    case START_SCAN:
        // scanEndFlag = true;
        // uint8_t cmd[20] = "scan";

        if(!scanEndFlag) {
            TextBox* text_box = uapp->text_box;
            text_box_reset(text_box);
            furi_string_reset(uapp->text_box_store);
            text_box_set_text(text_box, furi_string_get_cstr(uapp->text_box_store));

            uart_send_cmd(uapp, (uint8_t*)"scan\n", 5);

            if(uapp->scan_timer == NULL) {
                start_blink(ctx);
                uapp->scan_timer = furi_timer_alloc(scan_console_recv, FuriTimerTypePeriodic, ctx);
                furi_timer_start(uapp->scan_timer, 200);
            }
        }

        // while(!(strlen((char *)uapp->rx_buf) > 0));

        // furi_string_printf(uapp->text_box_store, "recv buf:%s,;", uapp->rx_buf);

        // TextBox* text_box = uapp->text_box;
        // text_box_set_text(text_box, furi_string_get_cstr(uapp->text_box_store));
        // stop_blink(app);
        return true;
    case UART_INIT:
        FURI_LOG_I(TAG, "UART init by scan module, baudrate: %d", uapp->BAUDRATE);
        uart_init(uapp, uapp->BAUDRATE, UART_CH);

        return true;
    default:
        return false;
    }
    return true;
}

static bool console_custom_event_callback(uint32_t event, void* context) {
    bleScanCtx* ctx = (bleScanCtx*)context;

    char buf[20] = "";
    switch(event) {
    // clean logs
    case 1:
        // text_box_reset(_ctx->uapp->text_box);
        // view_dispatcher_switch_to_view(_ctx->view_dispatcher, subMenuViewConsole);

        furi_string_printf(ctx->uapp->text_box_store, "%s", buf);
        text_box_set_text(ctx->uapp->text_box, furi_string_get_cstr(ctx->uapp->text_box_store));
        return true;
    case 2:
        FURI_LOG_I(TAG, "UART init by console module, baudrate: %d", ctx->uapp->BAUDRATE);
        uart_init(ctx->uapp, ctx->uapp->BAUDRATE, UART_CH);
        return true;
    default:
        return false;
    }
    return true;
}

//扫描界面按键监听
static bool view_scan_input_callback(InputEvent* input_event, void* context) {
    bleScanCtx* _ctx = (bleScanCtx*)context;
    // uart_app *uapp = _ctx->uapp;

    if(input_event->type == InputTypeShort) {
        if(input_event->key == InputKeyLeft) {
            view_dispatcher_send_custom_event(_ctx->view_dispatcher, TEST_BTN); //左键为测试按钮
            return true;
        } else if(input_event->key == InputKeyOk) {
            view_dispatcher_send_custom_event(
                _ctx->view_dispatcher, START_SCAN); //确认键为开始扫描按钮
            return true;
        }
    }
    return false;
}

static bool view_console_input_callback(InputEvent* input_event, void* context) {
    bleScanCtx* _ctx = (bleScanCtx*)context;
    if(input_event->type == InputTypeShort) {
        if(input_event->key == InputKeyLeft) {
            view_dispatcher_send_custom_event(_ctx->view_dispatcher, 42);
            return true;

            // clean uart log
        } else if(input_event->key == InputKeyOk) {
            // view_dispatcher_switch_to_view(_ctx->view_dispatcher, subMenuViewConsole);
            view_dispatcher_send_custom_event(_ctx->view_dispatcher, 1);
            return true;
        }
    }
    return false;
}

void uart_terminal_console_output_handle_rx_data_cb(uint8_t* buf, size_t len, void* context) {
    furi_assert(context);
    uart_app* app = context;
    FuriString* new_str = app->reusable_str;
    furi_string_reset(new_str); // 重置字符串内容

    furi_string_cat_printf(new_str, "%s", buf);

    app->text_box_store_strlen += furi_string_size(new_str);
    while(app->text_box_store_strlen >= UART_TERMINAL_TEXT_BOX_STORE_SIZE - 1) {
        furi_string_right(app->text_box_store, app->text_box_store_strlen / 2);
        app->text_box_store_strlen = furi_string_size(app->text_box_store) + len;
    }

    furi_string_cat(app->text_box_store, new_str);
}

void uart_terminal_uart_set_handle_rx_data_cb(
    uart_app* uart,
    void (*handle_rx_data_cb)(uint8_t* buf, size_t len, void* context)) {
    furi_assert(uart);
    uart->handle_rx_data_cb = handle_rx_data_cb;
}

static void loop_console(void* app) {
    TextBox* text_box = ((uart_app*)app)->text_box;

    text_box_set_font(text_box, TextBoxFontText);
    text_box_set_focus(text_box, TextBoxFocusEnd);

    uart_terminal_uart_set_handle_rx_data_cb(
        ((uart_app*)app),
        uart_terminal_console_output_handle_rx_data_cb); // setup callback for rx thread

    text_box_set_text(text_box, furi_string_get_cstr(((uart_app*)app)->text_box_store));
}

static void view_console_enter_callback(void* context) {
    furi_assert(context);

    bleScanCtx* _ctx = (bleScanCtx*)context;

    uart_app* uapp = _ctx->uapp;

    if(uapp->uart_is_init == 0) {
        view_dispatcher_send_custom_event(_ctx->view_dispatcher, 2);
        uapp->uart_is_init = 1;
    }

    TextBox* text_box = uapp->text_box;
    // text_box_reset(text_box);

    text_box_set_font(text_box, TextBoxFontText);
    text_box_set_focus(text_box, TextBoxFocusEnd);

    furi_string_reset(uapp->text_box_store);

    // uart_terminal_uart_set_handle_rx_data_cb(
    //     ((uart_app *)uapp), uart_terminal_console_output_handle_rx_data_cb); // setup callback for rx thread
    text_box_set_text(text_box, furi_string_get_cstr(uapp->text_box_store));

    // 设置定时器，更新接收到的数据
    uapp->console_timer = furi_timer_alloc(loop_console, FuriTimerTypePeriodic, uapp);
    furi_timer_start(uapp->console_timer, 200);
}

void view_console_exit_callback(void* context) {
    bleScanCtx* _ctx = (bleScanCtx*)context;

    // UNUSED(_ctx);
    // Unregister rx callback
    uart_terminal_uart_set_handle_rx_data_cb(_ctx->uapp, NULL);
}

static void view_scan_enter_callback(void* context) {
    bleScanCtx* _ctx = (bleScanCtx*)context;
    uart_app* uapp = _ctx->uapp;

    if(uapp->uart_is_init == 0) {
        view_dispatcher_send_custom_event(_ctx->view_dispatcher, UART_INIT);
        uapp->uart_is_init = 1;
    }
}

static void view_scan_exit_callback(void* context) {
    bleScanCtx* _ctx = (bleScanCtx*)context;
    stop_blink(_ctx);
}

static uint32_t navigation_submenu_callback(void* _context) {
    UNUSED(_context);
    return subMenuViewMain;
}

static uint32_t navigation_exit_callback(void* _context) {
    UNUSED(_context);
    return VIEW_NONE;
}

static int32_t uart_worker(void* context) {
    uart_app* uart = (void*)context;

    while(1) {
        uint32_t events =
            furi_thread_flags_wait(WORKER_ALL_RX_EVENTS, FuriFlagWaitAny, FuriWaitForever);
        furi_check((events & FuriFlagError) == 0);
        if(events & WorkerEvtStop) break;
        if(events & WorkerEvtRxDone) {
            size_t len = furi_stream_buffer_receive(uart->rx_stream, uart->rx_buf, RX_BUF_SIZE, 0);
            if(len > 0) {
                if(uart->handle_rx_data_cb) uart->handle_rx_data_cb(uart->rx_buf, len, uart);
            }
        }
    }

    furi_stream_buffer_free(uart->rx_stream);

    return 0;
}

void uart_terminal_uart_on_irq_cb(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context) {
    uart_app* uart = (void*)context;

    if(event == FuriHalSerialRxEventData) {
        uint8_t data = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(uart->rx_stream, &data, 1, 0);
        furi_thread_flags_set(furi_thread_get_id(uart->rx_thread), WorkerEvtRxDone);
    }
}

void uart_init(uart_app* uapp, int baudrate, FuriHalSerialId ch) {
    FURI_LOG_I(TAG, "UART initialing...\n");
    uapp->rx_stream = furi_stream_buffer_alloc(320 * 3, 1);
    uapp->rx_thread = furi_thread_alloc();
    furi_thread_set_name(uapp->rx_thread, "UART_TerminalUartRxThread");
    furi_thread_set_stack_size(uapp->rx_thread, 1024);
    furi_thread_set_context(uapp->rx_thread, uapp);
    furi_thread_set_callback(uapp->rx_thread, uart_worker);

    furi_thread_start(uapp->rx_thread);

    uapp->BAUDRATE = baudrate;

    uapp->serial_handle = furi_hal_serial_control_acquire(ch);
    if(!uapp->serial_handle) {
        FURI_LOG_E(TAG, "Failed to acquire serial handle");
        return;
    }
    // furi_check(uapp->serial_handle);

    furi_hal_serial_init(uapp->serial_handle, uapp->BAUDRATE);

    furi_hal_serial_async_rx_start(uapp->serial_handle, uart_terminal_uart_on_irq_cb, uapp, false);

    uapp->reusable_str = furi_string_alloc();
}

void uart_terminal_uart_free(uart_app* uart) {
    furi_assert(uart);

    furi_thread_flags_set(furi_thread_get_id(uart->rx_thread), WorkerEvtStop);
    furi_thread_join(uart->rx_thread);
    furi_thread_free(uart->rx_thread);

    furi_hal_serial_deinit(uart->serial_handle);
    furi_hal_serial_control_release(uart->serial_handle);

    furi_string_free(uart->reusable_str);
    free(uart);
}

//设置菜单 波特率
char* BaudRate_strings[] = {"115200", "38400", "9600"};
static void baudRate_change_callback(VariableItem* item) {
    furi_assert(item);

    bleScanCtx* app = variable_item_get_context(item);
    uart_app* uapp = app->uapp;

    int index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, BaudRate_strings[index]);
    uapp->baudrate_index = index;

    FURI_LOG_I(TAG, "BaudRate changed: %s", BaudRate_strings[uapp->baudrate_index]);
    uapp->BAUDRATE = atoi(BaudRate_strings[uapp->baudrate_index]);
    // view_dispatcher_send_custom_event(app->view_dispatcher, 0);
}

//程序主界面
// 为应用程序分配内存
bleScanCtx* ble_init() {
    bleScanCtx* app = (bleScanCtx*)safe_malloc(sizeof(bleScanCtx));
    if(app == NULL) {
        return NULL;
    }

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();

    //view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    app->notification = furi_record_open(RECORD_NOTIFICATION);

    app->submenu = submenu_alloc();

    uart_app* uapp = (uart_app*)safe_malloc(sizeof(uart_app));
    if(uapp == NULL) {
        free(app); // 释放之前分配的内存
        return NULL;
    }

    // 默认波特率
    uapp->BAUDRATE = 9600;

    // uapp->scanEndFlag = false;

    app->uapp = uapp;

    // 添加主菜单setup list列表
    /*
    submenu_add_item(app->submenu, "Setup", subMenuSetup, skeleton_submenu_callback, app);
    submenu_add_item(app->submenu, "Scan", subMenuScan, skeleton_submenu_callback, app);
    submenu_add_item(app->submenu, "Weapon", subMenuWeapon, skeleton_submenu_callback, app);
    submenu_add_item(app->submenu, "Console", subMenuConsole, skeleton_submenu_callback, app);
    submenu_add_item(app->submenu, "About", subMenuAbout, skeleton_submenu_callback, app);
    */

    submenu_add_item(app->submenu, "模块设置", subMenuSetup, skeleton_submenu_callback, app);
    submenu_add_item(app->submenu, "扫描蓝牙设备", subMenuScan, skeleton_submenu_callback, app);
    submenu_add_item(app->submenu, "漏洞攻击", subMenuWeapon, skeleton_submenu_callback, app);
    submenu_add_item(app->submenu, "控制台", subMenuConsole, skeleton_submenu_callback, app);
    submenu_add_item(app->submenu, "关于", subMenuAbout, skeleton_submenu_callback, app);

    view_set_previous_callback(submenu_get_view(app->submenu), navigation_exit_callback);
    //添加视图-主菜单
    view_dispatcher_add_view(
        app->view_dispatcher, subMenuViewMain, submenu_get_view(app->submenu));
    view_dispatcher_switch_to_view(app->view_dispatcher, subMenuViewMain);

    // 添加子菜单设置列表
    app->variable_item_list = variable_item_list_alloc();
    variable_item_list_reset(app->variable_item_list);

    VariableItem* credits =
        variable_item_list_add(app->variable_item_list, "确认已插入", 1, NULL, NULL);
    variable_item_set_current_value_text(credits, "Ble_ext");

    /*
    VariableItem* item = variable_item_list_add(
        app->variable_item_list,
        "BAUDRATE", // label to display
        COUNT_OF(BaudRate_strings), // number of choices
        baudRate_change_callback, // callback
        app); // context [use variable_item_get_context(item) to access]
    */
    //VariableItem* item = variable_item_list_add(app->variable_item_list, "BAUDRATE", COUNT_OF(BaudRate_strings), baudRate_change_callback, app);
    VariableItem* item = variable_item_list_add(
        app->variable_item_list,
        "通信波特率",
        COUNT_OF(BaudRate_strings),
        baudRate_change_callback,
        app);
    variable_item_set_current_value_index(item, 0);
    variable_item_set_current_value_text(item, BaudRate_strings[0]);

    view_set_previous_callback(
        variable_item_list_get_view(app->variable_item_list), navigation_submenu_callback);
    view_set_custom_callback(
        variable_item_list_get_view(app->variable_item_list), setup_custom_event_callback);

    //添加视图-设置菜单
    view_dispatcher_add_view(
        app->view_dispatcher,
        subMenuViewSetup,
        variable_item_list_get_view(app->variable_item_list));

    // app->view_list[subMenuViewSetup] = view_alloc();
    app->view_list[subMenuViewScan] = widget_alloc();
    app->view_list[subMenuViewWeapon] = view_alloc();
    uapp->text_box = text_box_alloc();
    app->view_list[subMenuViewConsole] = uapp->text_box;
    // app->view_list[subMenuViewConsole] = view_alloc();
    app->view_list[subMenuViewAbout] = widget_alloc();

    //////////兼容性
    app->text_box_view = text_box_get_view(uapp->text_box);
    //添加视图-控制台
    view_dispatcher_add_view(app->view_dispatcher, subMenuViewConsole, app->text_box_view);
    //////////兼容性

    uapp->text_box_store = furi_string_alloc();
    furi_string_reserve(uapp->text_box_store, UART_TERMINAL_TEXT_BOX_STORE_SIZE);

    //////////兼容性
    app->scan_view = widget_get_view(app->view_list[subMenuViewScan]);
    app->scan_view = view_alloc();
    //添加视图-扫描界面
    view_dispatcher_add_view(app->view_dispatcher, subMenuViewScan, app->scan_view);
    //////////兼容性

    view_set_context(app->scan_view, app); //scan_view扫描界面
    view_set_draw_callback(app->scan_view, view_scan_draw_callback);
    view_set_input_callback(app->scan_view, view_scan_input_callback);
    view_set_previous_callback(app->scan_view, navigation_submenu_callback);
    view_set_enter_callback(app->scan_view, view_scan_enter_callback);
    view_set_exit_callback(app->scan_view, view_scan_exit_callback);
    view_set_custom_callback(app->scan_view, scan_custom_event_callback);
    view_allocate_model(app->scan_view, ViewModelTypeLockFree, sizeof(SkeletonScanModel));

    SkeletonScanModel* scan_model = view_get_model(app->scan_view);
    scan_model->widget = app;
    scan_model->s = furi_string_alloc();
    furi_string_printf(
        scan_model->s,
        "%s",
        "This is a bluetooth ble scanner and controller tool.\n---\nScan any low energy ble device around.\nScan any low energy ble device around.\nScan any low energy ble device around.");

    //关于界面
    widget_add_text_scroll_element(
        app->view_list[subMenuViewAbout],
        0,
        1,
        128,
        64,
        //"This is a bluetooth ble scanner and something ble devices controller tool.\n---\nScan any low energy ble device around.");
        "该程序提供了BLE 蓝牙扫描\n"
        "功能，以及一些蓝牙设备的\n"
        "漏洞攻击功能。\n\n"
        "作者:H4lo(国内安全领域达人)\n"
        "兼容性及汉化:宅人改造家");

    view_set_context(app->text_box_view, app); //text_box_view文本输入界面？
    view_set_previous_callback(app->text_box_view, navigation_submenu_callback);
    view_set_enter_callback(app->text_box_view, view_console_enter_callback);
    view_set_exit_callback(app->text_box_view, view_console_exit_callback);
    view_set_input_callback(app->text_box_view, view_console_input_callback);
    view_set_custom_callback(app->text_box_view, console_custom_event_callback);

    //////////兼容性
    app->weapon_view = app->view_list[subMenuViewWeapon];
    //添加视图-武器界面
    view_dispatcher_add_view(app->view_dispatcher, subMenuViewWeapon, app->weapon_view);
    //////////兼容性

    view_set_context(app->weapon_view, app); //weapon_view攻击界面？
    view_set_draw_callback(app->weapon_view, view_weapon_draw_callback);
    view_set_input_callback(app->weapon_view, view_weapon_input_callback);
    view_set_custom_callback(app->weapon_view, weapon_custom_event_callback);
    // view_set_exit_callback(app->weapon_view, view_console_exit_callback);
    view_set_previous_callback(app->weapon_view, navigation_submenu_callback);

    // draw callback 必须使用view_allocate_model
    view_allocate_model(app->weapon_view, ViewModelTypeLockFree, sizeof(SkeletonWeaponModel));
    SkeletonWeaponModel* model = view_get_model(app->weapon_view);

    model->state_index = 0;
    model->is_start = false;

    view_set_previous_callback(
        widget_get_view(app->view_list[subMenuViewAbout]), navigation_submenu_callback);

    //添加视图-关于界面
    view_dispatcher_add_view(
        app->view_dispatcher, subMenuViewAbout, widget_get_view(app->view_list[subMenuViewAbout]));

    return app;
}

// 释放程序所使用的内存
void ble_free(bleScanCtx* app) {
    if(app == NULL) {
        return;
    }

    uart_app* uapp = app->uapp;

    // 释放 device 数组的内存
    for(int i = 0; i < MAX_DEVICE_SCAN; i++) {
        if(device[i] != NULL) {
            free(device[i]);
            device[i] = NULL;
        }
    }

    // 释放 SkeletonScanModel 中的字符串资源
    SkeletonScanModel* scan_model = view_get_model(app->scan_view);
    if(scan_model != NULL && scan_model->s != NULL) {
        furi_string_free(scan_model->s);
    }

    // 释放视图
    view_dispatcher_remove_view(app->view_dispatcher, subMenuViewMain);
    view_dispatcher_remove_view(app->view_dispatcher, subMenuViewSetup);
    view_dispatcher_remove_view(app->view_dispatcher, subMenuViewScan);
    view_dispatcher_remove_view(app->view_dispatcher, subMenuViewWeapon);
    view_dispatcher_remove_view(app->view_dispatcher, subMenuViewConsole);
    view_dispatcher_remove_view(app->view_dispatcher, subMenuViewAbout);

    // 释放定时器资源
    if(uapp->console_timer) {
        furi_timer_free(uapp->console_timer);
    }
    if(uapp->scan_timer) {
        furi_timer_free(uapp->scan_timer);
    }

    // 释放文本框和字符串资源
    text_box_free(uapp->text_box);
    furi_string_free(uapp->text_box_store);

    // 释放 UART 相关资源
    if(uapp->uart_is_init != 0) {
        uart_terminal_uart_free(uapp);
    }

    // 释放 widget 资源
    widget_free(app->view_list[subMenuViewAbout]);
    widget_free(app->view_list[subMenuViewScan]);

    // 释放视图资源
    view_free(app->view_list[subMenuViewWeapon]);
    view_free(app->view_list[subMenuViewConsole]);

    // 释放菜单和视图调度器资源
    submenu_free(app->submenu);
    variable_item_list_free(app->variable_item_list);
    view_dispatcher_free(app->view_dispatcher);

    // 关闭记录
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    // 释放应用程序上下文资源
    free(app);
    scanEndFlag = false;
}

// 启动应用程序的主函数
int32_t main_entry() {
    //加载程序主界面
    bleScanCtx* app = ble_init(); //为应用程序分配内存

    if(app == NULL) {
        return -1;
    }

    FURI_LOG_I(TAG, "FINISH ble_init.");

    view_dispatcher_run(app->view_dispatcher); //加载视图调度器

    ble_free(app); //释放程序所使用的内存

    return 0; //关闭程序
}
