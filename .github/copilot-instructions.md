# Quy chuẩn dự án Nhúng ESP32 (PlatformIO)

## 1. Cấu hình Phần cứng & Môi trường
- **Vi điều khiển:** ESP32 (hoặc ESP32-S3 / ESP32-C3 - ghi rõ dòng chip bạn đang dùng).
- **Build System:** PlatformIO (`platformio.ini` ở thư mục gốc).
- **Framework:** Arduino / FreeRTOS tích hợp sẵn.
- **Sơ đồ chân (Pinout) cố định của dự án:**
  - I2C: SDA = GPIO 21, SCL = GPIO 22
  - RS485 / Modbus UART2: RX = GPIO 16, TX = GPIO 17
  - Lưu ý phần cứng: Chân GPIO 34, 35, 36, 39 chỉ nhận tín hiệu vào (Input-only) và không có trở kéo nội (No internal pull-up/pull-down). Không dùng các chân ADC2 khi đang bật Wi-Fi.

## 2. Quy tắc viết code C/C++ cho ESP32 (Bắt buộc)
- **Đa nhiệm (FreeRTOS) & Chống treo mạch:** Tuyệt đối KHÔNG dùng hàm `delay()` chặn luồng dài trong vòng lặp chính hoặc các tác vụ điều khiển. Hãy dùng `vTaskDelay(pdMS_TO_TICKS(ms))` hoặc bộ đếm `millis()`.
- **Quản lý bộ nhớ (RAM/Heap):** Tránh tối đa việc nối chuỗi bằng đối tượng `String` của Arduino trong vòng lặp liên tục gây phân mảnh bộ nhớ Heap. Ưu tiên dùng mảng ký tự tĩnh `char[]` với `snprintf()`.
- **Hàm ngắt (ISR):** Mọi hàm phục vụ ngắt (`attachInterrupt`) bắt buộc phải có thuộc tính `IRAM_ATTR`, viết cực gọn, không dùng `Serial.print` hay `delay` bên trong ISR.
- **Xử lý lỗi & Log:** Sử dụng macro `ESP_LOGI`, `ESP_LOGE` hoặc `Serial.printf` có kèm nhãn thời gian/module rõ ràng để dễ debug.

## 3. Quy trình làm việc trên Terminal (Dành cho Copilot Agent)
- **Tự động kiểm tra biên dịch:** Mỗi khi sửa xong code trong `src/` hoặc `include/`, hãy tự chạy lệnh sau trong Terminal để kiểm tra lỗi biên dịch và dung lượng RAM/Flash:
  `pio run`
- Nếu lệnh `pio run` báo lỗi biên dịch (compilation error) hoặc thiếu thư viện, hãy tự đọc log trên Terminal, sửa code cho đến khi biên dịch thành công (`[SUCCESS]`).
- **Quy tắc an toàn phần cứng:** KHÔNG tự ý chạy lệnh nạp code xuống mạch (`pio run -t upload`) hoặc xóa Flash (`erase`) nếu người dùng chưa yêu cầu rõ ràng.