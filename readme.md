# uLogger WiFi Example

This example demonstrates how to integrate uLogger into a WiFi application running on a Silicon Labs SiWG917 device. The embedded firmware logs binary debug data over MQTT to publish to the uLogger cloud platform.

## Prerequisites

- [Simplicity Studio 5](https://www.silabs.com/developers/simplicity-studio) with the Gecko SDK installed
- Python 3.9 or greater
- A supported Silicon Labs radio board (BRD4002A with SiWG917 module, BRD2605A)
- A [uLogger](https://www.ulogger.ai) account
- GNU ARM Toolchain - GNU ARM v12.2.1
- Silicon Labs Simplicity SDK 2025.6.2
- WiseConnect SDK 3.5.2

---

## 1. Import the demo example into your worksapce

1. In Simplicity studio, click File->Import
2. Browse to folder where you checked out the uLogger example-wifi-demo
3. Select the Simplicity Studio (.sls) project and click next
4. Make Sure the SDK and toolchain text is not showing red. If it is red, that means you'll need to download those packages in the package manager. Click next.
5. Uncheck the box for `Use default location` and change the location to point the location where you downloaded the repo. It should include the name of the folder itself. An example would be `C:\my-demo-app\example-wifi-demo`
6. Click Finish to complete the import.


---

## 2. Create Your uLogger Account

1. Go to [ulogger.ai](https://www.ulogger.ai) and create an account.
2. Log in and follow the setup instructions to create a test application.
3. After creating the application, copy the **Application ID** displayed in the web application.

---

## 3. Configure the Firmware

Open the file `config/sl_net_default_values.h`. Set the field `DEFAULT_WIFI_CLIENT_PROFILE_SSID` to your access point SSID and `DEFAULT_WIFI_CLIENT_CREDENTIAL` to the access point passphrase.

All locations that require your credentials are marked with the comment pattern `ULOGGER TODO`. Search for this pattern in the project to find every location you need to update.

### Set the Application ID

Open `include/ulogger_config.h` and update the `APPLICATION_ID` define with the Application ID from your uLogger cloud account:

```c
// ULOGGER TODO
#define APPLICATION_ID <your_application_id>
```

---

## 4. Configure the Python Script

### Download Your MQTT Certificates

1. Log in to the uLogger web application.
2. Click **Settings** in the navigation panel.
3. Click **Download MQTT Certificate**.
4. Extract the downloaded zip file and copy the following two files into the project directory:
   - `certificate.pem.crt`
   - `private.pem.key`

### Set Your Customer ID

Open `wifi_example.json` and update the `customer_id` and `application_id` to match the values that you recorded in steps 3 and 4.

---

## 5. Publish the AXF File to uLogger Cloud

The `.axf` file contains the debug symbols needed to decompress binary logs on the cloud. This file must be uploaded after each firmware build, and is called automatically as a project post-build step.

1. Download the uLogger upload client from [https://ulogger.ai/downloads.html](https://ulogger.ai/downloads.html).

---

## 6. Build and Flash
Build the Simplicity Studio project and flash it to your device. 

## 7. Generate Logs
Follow the log instructions that are displayed via the debug output on the virtual com port. You can generate logs and a hard fault by pressing BUTTON0.

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