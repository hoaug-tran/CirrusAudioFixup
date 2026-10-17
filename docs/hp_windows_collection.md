# Thu thập Windows audio và CS35L41

`Tools/Collect-HP-CS35L41.ps1` chạy trên Windows PowerShell 5.1 x64. Tên file được giữ để tương thích; việc chọn dữ liệu dựa trên thiết bị hiện có, không cố định model HP, số OEM INF hay tên firmware.

Mở Windows PowerShell **Run as administrator**, chuyển đến thư mục chứa script và chạy:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Collect-HP-CS35L41.ps1 -SpeakerTestNotes 'Speakers: ...; headphones: ...; cold boot: ...; after sleep: ...'
```

Có thể chạy launcher `.cmd` với cùng tham số. Mặc định kết quả nằm trong `%PUBLIC%\Documents\Windows-Audio-...` và file ZIP bên cạnh. Không cần cài công cụ bên ngoài. Script không cài driver, restart thiết bị, sửa registry, ghi firmware hay upload dữ liệu.

## Phạm vi

- Thiết bị âm thanh đang hiện diện, endpoint phát/thu, codec và Cirrus AMP; quan hệ cha, controller SPI/I2C/GPIO liên quan được ghi để phân tích topology.
- Driver đang gắn với các thiết bị được chọn; extension INF khớp chính xác hardware ID, software component/APO có quan hệ cụ thể và OEM Include dependency. Extension khớp ID được ghi là ứng viên, không coi là bằng chứng nó đang hoạt động.
- Toàn bộ file của đúng package DriverStore đã xác định, cùng binary dịch vụ/APO được tham chiếu trực tiếp. Không export toàn bộ DriverStore, mọi SoftwareComponent hay mọi driver cùng hãng.
- Thuộc tính PnP, tài nguyên thiết bị, registry của thiết bị/class/service và endpoint audio; đoạn SetupAPI liên quan, sự kiện audio gần đây có lọc.
- Đọc riêng biến EFI `CirrusSmartAmpCalibrationData` nếu truy cập được. Không dump ACPI, SSDT, DSDT hay toàn bộ biến firmware.

`devices/selection.json` và `packages/selection.json` giải thích lý do chọn. Ancestor chung chỉ dùng mô tả topology, không mở rộng thành thu thập mọi package chipset/GPU.

## Dung lượng và tính đầy đủ

Mặc định ngân sách **payload duy nhất là 512 MiB**, không phải giới hạn kích thước ZIP. File trùng nội dung chỉ lưu một lần trong `payload/`; `payload-plan.csv` ánh xạ tên và đường dẫn gốc sang blob SHA-256. Giữ CSV này cùng payload khi phân tích hoặc khôi phục cấu trúc package. Script kiểm tra lại hash sau khi copy.

Nếu vượt ngân sách, script vẫn ghi metadata và kế hoạch nhưng **không copy payload**, báo `OVER_BUDGET` và thoát mã 2. Không cắt bớt firmware hay package âm thầm. Xem `payload-size.json`, lý do chọn package và các file lớn trước khi tăng giới hạn:

```powershell
.\Collect-HP-CS35L41.ps1 -PlanOnly -NoZip
.\Collect-HP-CS35L41.ps1 -MaxPayloadMB 1024
```

`-PlanOnly` kiểm tra lựa chọn và hash package, bỏ qua registry/log/calibration và copy payload. `-NoZip` giữ thư mục kết quả. `-EventDays` mặc định 3 ngày, nhận 1–30; `-OutputRoot` đổi nơi lưu. Mỗi lần chạy tạo thư mục mới.

Đọc `status.csv` trước khi gửi kết quả: mã 2 nghĩa là có dữ liệu thiếu, lỗi hoặc giới hạn đã chạm; không chạy Administrator thường thiếu ánh xạ DriverStore. Mục `UNAVAILABLE` ghi rõ dữ liệu không hiện diện, không phải bằng chứng phần cứng hỏng. Windows cũ dùng tài nguyên CIM nếu không có tùy chọn PnPUtil tương ứng. Không còn tham số export mọi driver.

ZIP có thể chứa định danh thiết bị, đường dẫn và cấu hình âm thanh; xem lại trước khi chia sẻ. Dữ liệu thu thập hỗ trợ phân tích driver, không tự chứng minh kext macOS đã phát tiếng hoặc hỗ trợ đầy đủ một máy.

Tham khảo: [PnPUtil](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-command-syntax), [Get-WindowsDriver](https://learn.microsoft.com/en-us/powershell/module/dism/get-windowsdriver?view=windowsserver2025-ps), [extension INF](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/using-an-extension-inf-file).
