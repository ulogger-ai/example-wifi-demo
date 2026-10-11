# uLogger WiFi Example

This example demonstrates how to integrate uLogger into a WiFi application running on a Silicon Labs SiWG917 device. The embedded firmware logs binary debug data over MQTT to publish to the uLogger cloud platform.

The uLogger agent is vendored into this project: the headers in `include/` and the static library in `lib/` are copied from the [embedded_agent](https://github.com/ulogger-ai/embedded_agent) distribution. The pinned version is in `include/ulogger_version.h` — currently **v1.2.6**.

## Prerequisites

- [Simplicity Studio 6](https://www.silabs.com/developers/simplicity-studio), or VS Code with the Silicon Labs extension
- A supported Silicon Labs radio board (BRD4002A with SiWG917 module, BRD2605A)
- A [uLogger](https://www.ulogger.ai) account
- GNU Arm Toolchain 14.2.rel1
- Silicon Labs Simplicity SDK 2026.6.1
- WiSeConnect SDK 4.1.1
- SiWG917 network processor (NWP) firmware 2.16.5.1.0.8 on the board — see [Update the NWP firmware](#update-the-nwp-firmware)

Install the SDKs and toolchain through the Simplicity Installer.

---

## 1. Generate the project

The repository does not contain a ready-to-build project: `autogen/` and the CMake files in `cmake_gcc/` are regenerated for each machine, and `cmake_gcc/example-wifi-demo.cmake` (which holds absolute SDK paths) is not committed. Generate it from `example-wifi-demo.slcp` before the first build.

- **Simplicity Studio 6 / VS Code:** open the folder where you checked out this repository and generate the project from `example-wifi-demo.slcp`. Studio 6 produces a CMake build in `cmake_gcc/`.
- **Command line:** with `slc-cli` installed (`slt install slc-cli`), run from the repository folder:

  ```bash
  slc generate -p example-wifi-demo.slcp -d . -o cmake -tlcn gcc \
      --sdk-package-path=<simplicity_sdk path>,<wiseconnect path> --tt
  ```

Generation rewrites the files in `autogen/` and `cmake_gcc/autogen_toolchain.cmake` with paths for your machine. Do not commit those changes. It keeps your files in `config/`, but check `git diff config/` afterwards: an SDK update can replace a customized config file with the SDK default (this happened to `config/aws_iot_config.h` during the Simplicity Studio 6 migration).


---

## 2. Create Your uLogger Account

1. Go to [ulogger.ai](https://www.ulogger.ai) and create an account.
2. Log in and follow the setup instructions to create a test application.
3. After creating the application, copy the **Application ID** displayed in the web application.

---

## 3. Configure the Firmware

Open the file `config/sl_net_default_values.h`. Set the field `DEFAULT_WIFI_CLIENT_PROFILE_SSID` to your access point SSID and `DEFAULT_WIFI_CLIENT_CREDENTIAL` to the access point passphrase.

All locations that require your credentials are marked with the comment pattern `ULOGGER TODO`. Search for this pattern in the project to find every location you need to update.

### Set the Customer and Application IDs

Open `include/ulogger_config.h` and set `ULOGGER_CUSTOMER_ID` and `ULOGGER_APPLICATION_ID` to the values from your uLogger cloud account. If your account uses groups, also set `ULOGGER_GROUP_ID` to the group your device belongs to (use `0` if you are not using groups):

```c
// ULOGGER TODO
#define ULOGGER_CUSTOMER_ID           <your_customer_id>
#define ULOGGER_APPLICATION_ID        <your_application_id>
#define ULOGGER_GROUP_ID              <your_group_id>
```

The repository ships placeholder values (`12345`), and `app.c` stops the build with an `#error` until you replace them.

Your IDs, like the Wi-Fi credentials above, are specific to you and must not be committed. To keep git from picking up your local edits to these two files:

```bash
git update-index --skip-worktree include/ulogger_config.h config/sl_net_default_values.h
```

(Undo with `--no-skip-worktree` before pulling an upstream change to either file.)

---

## 4. Configure the Credentials

### Download Your MQTT Certificates

1. Log in to the uLogger web application.
2. Click **Settings** in the navigation panel.
3. Click **Download MQTT Certificate**.
4. Extract the downloaded zip file and copy the following three files into the project directory:
   - `certificate.pem.crt`
   - `private.pem.key`
   - `ulogger_certs_keys.h`

`ulogger_certs_keys.h` is gitignored. The firmware does not build without it.

> Note: the upload in step 5 takes your customer and application IDs from environment variables, so `wifi_example.json` keeps its placeholders.

> Note: MQTT topics now include a `group_id` segment (`.../{customer_id}/{group_id}/{application_id}/...`). The `group_id` used at runtime comes from the `ULOGGER_GROUP_ID` define in `include/ulogger_config.h`, not from `wifi_example.json`.

---

## 5. Publish the AXF File to uLogger Cloud

The AXF file carries the debug symbols the cloud needs to decode this firmware's binary logs. The cloud matches them to incoming logs on the **version string**, so it has to be re-uploaded whenever you change the firmware or bump `.version_string` in `app.c`. A stale AXF fails quietly: logs arrive and simply cannot be decoded, with nothing to say why.

Under Simplicity Studio 6 the build output is `cmake_gcc/build/base/example-wifi-demo.out` — the same ELF-with-symbols, just renamed from `.axf`. `wifi_example.json` already points at it.

1. Download the uLogger upload client from [https://ulogger.ai/downloads.html](https://ulogger.ai/downloads.html).

2. Put your MQTT certificates in the project directory. Download the bundle from the uLogger web app (or `get_certificate_bundle` over MCP) and unpack `certificate.pem.crt` and `private.pem.key` here. They are gitignored.

   The upload authenticates with these, so a certificate belonging to a different customer fails with a bare `403 Forbidden` that says nothing about which account it expected.

3. Set your account identifiers. These override the placeholders in `wifi_example.json`, so you do not have to edit (or accidentally commit) them:

   ```bash
   export ULOGGER_CUSTOMER_ID=<your customer id>
   export ULOGGER_APPLICATION_ID=<your application id>
   ```

4. To upload by hand:

   ```bash
   ulogger-upload -json wifi_example.json -project_dir . \
       -customer_id $ULOGGER_CUSTOMER_ID -application_id $ULOGGER_APPLICATION_ID
   ```

   To have it run automatically after every build, point `ULOGGER_UPLOAD` at the client and reconfigure:

   ```bash
   export ULOGGER_UPLOAD=/path/to/ulogger-upload
   cmake --preset <your preset>      # or -DULOGGER_UPLOAD=/path/to/ulogger-upload
   ```

   With `ULOGGER_UPLOAD` unset the step is skipped and the build is unaffected. With it set, a failed upload fails the build — having opted in, a silent miss is the outcome worth avoiding.

> Under Simplicity Studio 5 this ran from the Eclipse `Build Steps` project setting. SS6 builds through CMake and does not carry that setting over, so it now lives in `cmake_gcc/CMakeLists.txt`.

---

## 6. Build and Flash

### Update the NWP firmware

The SiWG917 runs its Wi-Fi stack on a separate network processor (NWP) with its own firmware, and that firmware must match the WiSeConnect SDK. With WiSeConnect 4.1.1, a board running older NWP firmware (for example 2.14.x) joins the access point and gets a DHCP address, but `sl_net_up` then fails with `0x7` (`SL_STATUS_TIMEOUT`) after about 30 seconds and the app prints `Error while connecting to Access point: 0x7`.

Flash the matching image from the WiSeConnect SDK once per board:

```bash
commander flash <wiseconnect path>/connectivity_firmware/standard/SiWG917-B.2.16.5.1.0.8.rps \
    --serialno <J-Link serial> --device SIWG917M111MGTBA
```

The app prints the NWP version when its MQTT task starts: `Firmware version is: 1711.2.16.5.1.0.8`.

### Build and flash the application

Build the project (Simplicity Studio 6, or `cmake --preset project` followed by `cmake --build --preset default_config` in `cmake_gcc/`) and flash `cmake_gcc/build/base/example-wifi-demo.rps` to your device.

If `commander` reports `Could not connect debugger`, retry: the first attempt can fail while the debug adapter is busy.

### Debugging in VS Code

The generated `.vscode/launch.json` may need two fixes before debugging starts:

- `"gdbPath": "."` produces `Could not start GDB process ... spawn . EACCES`. Point it at `arm-none-eabi-gdb` from the GNU Arm 14.2.rel1 toolchain, and set `"serverpath"` to your `JLinkGDBServerCLExe` if it is empty.
- `"device": "SiWG917M111MGTBA"` makes the J-Link GDB server exit with `Failed to get index for device name`. Use the J-Link device name `SiWG917M111M` instead.

The extension rewrites `launch.json` when it regenerates, so re-check these after a regeneration.

## 7. Generate Logs
Follow the log instructions that are displayed via the debug output on the virtual com port (115200 baud). You can generate logs and a hard fault by pressing BUTTON0.

> **No output on the virtual COM port?** If the Simplicity Device Manager is running, its link daemon (`silink`) takes over the debug adapter, including the serial output, so nothing arrives on `/dev/cu.usbmodem…` (or the COM port on Windows). View the output in the Device Manager's console, or quit the Device Manager so the serial port receives it again.

## How this example uses uLogger

This section is not part of the setup walkthrough — it describes what the demo code does, and what to change when you adapt it to your own hardware. For the full integration guide see the [embedded_agent README](https://github.com/ulogger-ai/embedded_agent).

### The log transfer cycle

The publish path in `mqtt.c` follows a **seal → read → consume** cycle:

1. `ulogger_seal_nv_logs_for_transfer()` freezes the current NV contents and returns the size of that snapshot, which is used to size the publish buffer.
2. `ulogger_read_nv_logs_with_header()` fills the buffer — a 25-byte header followed by the log data — and it is published to the binary-log MQTT topic.
3. On a successful publish, `ulogger_consume_nv_logs()` discards the snapshot.

The last step uses `ulogger_consume_nv_logs()` rather than `ulogger_clear_nv_logs()` deliberately. Clearing erases the whole region, which would destroy any log written *while the publish was in flight*; consuming discards only the sealed snapshot.

> **Duplicates after a reset.** The consume position lives in RAM, so a reset re-offers everything still physically present in the NV region — including entries already delivered but not yet erased. Expect duplicates of up to a region's worth of entries after a reset. This is the deliberate trade for not losing entries written during a transfer. Note that tick values cannot be used to de-duplicate, because the tick counter restarts at 0 on reset.

### Non-volatile storage is simulated

**This demo does not use real non-volatile memory.** `app.c` backs both uLogger regions with a single static RAM buffer placed in `.noinit`:

| Region | Offset | Size | Purpose |
|---|---|---|---|
| `ULOGGER_MEM_TYPE_DEBUG_LOG` | `0` | 1500 bytes | Binary logs |
| `ULOGGER_MEM_TYPE_STACK_TRACE` | `1500` | 1100 bytes | Crash dumps |

`.noinit` means the contents survive a **reset**, which is what lets a crash dump be captured and then published after reboot — but they do **not** survive a power cycle. For a production port, replace `ulogger_nv_mem_read/write/erase()` with real flash routines and point the regions at a flash area reserved in your linker script.

`erase_granularity` is set to `NV_SIM_PAGE_SIZE` (100 bytes), a stand-in for a flash page. It switches the library onto page-wise reclaim, so a transfer stops discarding entries written while it was in flight. On real hardware, set this to the erasable unit of your part — the same constant your `erase()` driver steps by. The library only uses the value if `start_addr` *and* the region length are both multiples of it, otherwise it silently falls back to whole-region erases.

### Logging configuration

The demo logs at `ULOG_DEBUG` with all modules enabled, and the cloud can change both at runtime — `mqtt.c` subscribes to a config topic and applies updates via `ulogger_set_flags_level()`.

Note that `pretrigger_log_count` is **0** in this example, so the pretrigger buffer is effectively disabled and every log that passes the filter goes straight to NV. Raise it (and keep `pretrigger_buffer_size` large enough) if you want the dash-cam behaviour of retaining recent lower-severity logs and flushing them only when something goes wrong.

### Crash dumps

`fault_reboot_cb` runs once a dump is captured. As of agent v1.2.5 it takes the crash cause:

```c
static void fault_reboot(uint8_t cause) {
    (void)cause;   // one of ULOGGER_CRASH_CAUSE
    NVIC_SystemReset();
}
```

This demo resets the same way for every cause; an application that needs to recover differently from a watchdog bite than from a CPU fault can branch on `cause` here.

### Low-power sleep is disabled

The `sl_power_manager` component brings in tick-less idle, which puts the M4 to sleep whenever no task is running. In this demo that is almost always: the main thread spends its time waiting for BTN0. A press while the core is asleep only wakes it — it never reaches `sl_si91x_button_isr`, so the button appears to do nothing.

`main_application()` in `app.c` therefore holds a PS4 requirement (`sl_si91x_power_manager_add_ps_requirement(SL_SI91X_POWER_MANAGER_PS4)`) so the core never sleeps. This also keeps the debugger able to attach. A battery-powered product that needs the sleep should instead set the button event from the NPSS GPIO wake-up path.

### `ulogger_config.h` is yours

`include/ulogger_config.h` holds your module list, application ID, group ID and NV addresses. It is **not** shipped by the agent distribution, so updating the vendored library will never overwrite your settings.

---

## Exploring the web platform
Now that you have successfully published logs into uLogger Cloud, let's review how to view those.

### Hard Faults
1. Click on **Hard Faults** on the left navigation panel. You should see at least one hard fault in the list.
2. Click on the error summary text to expand the entry. You should see the full stack trace, hard fault status registers, and general registers.

### Debug Logs
1. Click on **Debug Logs** on the left navigation panel. You should see at least one error message in the list. Click the error text to expand the log message.
2. Click on the device address of your specific device to open the log history for that specific device. You should now see several logs captured prior to the error message and hard fault.

### Fix with AI
If you forked this repo and provide your github settings, you can send this context to your AI coding agent to analyze and create a pull request to fix for you. Even if you haven't configured your github settings, you can still follow the steps even though the pull request won't get generated.
1. Hold shift and click on the first log that you want to be included in the context and then the last log. This selects the range of interest if you don't want to send all logs as part of the context.
2. Click the Fix with AI button in the top right corner. This will display a new modal with your selected log context and the option to provide additional user context if desired.
3. Click submit to create the issue for the AI tools to analyze and fix.

There is more to explore in the web platform, but that is all that is covered in this demo. Check some of our other demos and videos to learn more!