
# UdemySaver
![GitHub release (latest by date)](https://img.shields.io/github/v/release/Jadis0x/UdemySaver?color=75a9ff&style=flat-square)
![C++](https://img.shields.io/badge/language-C%2B%2B20-00599C?style=flat-square&logo=c%2B%2B)
![GitHub Repo stars](https://img.shields.io/github/stars/Jadis0x/UdemySaver?style=flat-square)
![GitHub repo size](https://img.shields.io/github/repo-size/Jadis0x/UdemySaver?style=flat-square)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20a%20Coffee-Donate-yellow?style=flat-square&logo=buy-me-a-coffee)](https://www.buymeacoffee.com/jadis0x)

UdemySaver is an open-source Udemy course downloader written in modern C++20. It lets you save videos, subtitles, and course assets you own for offline learning. 
A built-in HTTP server powers a responsive web interface so you can browse your Udemy library, queue downloads, and monitor progress from any browser.

## Features
- Download Udemy courses, lectures, subtitles, and attachments for offline access.
- Responsive web interface to browse your Udemy library.
- Queue and monitor downloads for single lectures or entire courses.
- Automatically organizes files under a structured `downloads/` directory.
- Cross-platform build via CMake for Windows, macOS, and Linux.

## Showcase
![Course library view](app_showcase/App_Screenshot_2.png)

# Requirements
| Tool/Library           | Notes                     |
| ---------------------- | ------------------------- |
| C++20 compiler         | MSVC / Clang / GCC        |
| CMake 3.20 or newer     | Build system              |
| FFmpeg + pkg-config     | Native video processing dependency |

## Build from source

### Windows
```bash
git clone https://github.com/jadis0x/UdemySaver.git
cd UdemySaver
cmake -S . -B build
cmake --build build
```

### macOS
On macOS, install the native FFmpeg dependency with Homebrew. Curl, Boost and
nlohmann_json are downloaded automatically by CMake for the active CPU architecture.

```bash
# Install dependencies
brew install ffmpeg pkg-config cmake

# Clone and build
git clone https://github.com/jadis0x/UdemySaver.git
cd UdemySaver
rm -rf build # required if this directory was previously configured as x86_64
cmake -S . -B build -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build
```

The executable will be at `build/UdemySaver/UdemySaver`

## Configuration
Create a settings.ini alongside the executable:
```ini
# UdemySaver settings
udemy_access_token=PASTE_YOUR_TOKEN_HERE
udemy_client_id=PASTE_YOUR_CLIENT_ID_HERE
udemy_api_base=https://www.udemy.com
http_proxy=http://127.0.0.1:8888
download_subtitles=true
download_assets=true
```

DRM-protected DASH lectures are preserved as separate encrypted video and audio
fragmented-MP4 files. UdemySaver downloads the MPD initialization and media
segments directly with libcurl. It does not request a DRM license, decrypt the
result, or make the downloaded files playable.
You can also start the program without a token and paste it via the web interface; the file will be created automatically.

**Note:** Due to Cloudflare protection on Udemy's API, you need to use a proxy like mitmproxy to bypass bot detection. See the "Cloudflare Bypass" section below.

## Obtaining the access token and client_id

1. Log in to Udemy in your browser.
2. Open the developer tools (F12) and head to the Application/Storage tab.
3. Under **Cookies** for `www.udemy.com`:
   - Copy the **entire** value of `access_token` (including the colon `:` in the middle)
   - Copy the value of `client_id`
4. Use these values in `settings.ini` or paste the access token into the web UI when prompted.

**Important:** The `access_token` cookie has TWO parts separated by a colon (format: `public_key:private_key`). You must copy the complete value including both parts.

## Usage

1. Run the application:

```bash
./UdemySaver
```

2. Open a browser at <http://127.0.0.1:8080>.
3. If prompted, paste your Udemy access token.
4. Browse your library, choose a course, and click **Download**.
5. Files are saved under a `downloads/` directory.

## Demo Video
[![UdemySaver Demo](https://img.youtube.com/vi/XLbq7OacdHE/0.jpg)](https://youtu.be/XLbq7OacdHE)

## Cloudflare Bypass

Udemy uses Cloudflare protection to block automated API requests. To bypass this, you need to route UdemySaver through a local proxy like **mitmproxy**.

### Setup with mitmproxy

1. **Install mitmproxy:**
   ```bash
   # macOS
   brew install mitmproxy

   # Linux
   pip install mitmproxy

   # Windows
   # Download from https://mitmproxy.org
   ```

2. **Start mitmproxy:**
   ```bash
   mitmproxy --listen-port 8888
   ```
   Keep this running in a separate terminal.

3. **Configure settings.ini:**
   Add the proxy setting to your `settings.ini`:
   ```ini
   http_proxy=http://127.0.0.1:8888
   ```

4. **Run UdemySaver:**
   ```bash
   cd build/UdemySaver
   ./UdemySaver
   ```

5. **Access the web interface:**
   Open http://127.0.0.1:8080 in your browser.

The proxy will intercept and forward all requests, bypassing Cloudflare's bot detection. SSL certificate verification is automatically disabled when using a proxy.

### Why is this needed?

Udemy's Cloudflare protection uses:
- TLS fingerprinting to detect non-browser clients
- HTTP/2 fingerprinting
- Browser-specific headers (sec-ch-ua)

Routing through mitmproxy makes requests appear as if they're coming from a legitimate browser, allowing downloads to work.

## macOS-Specific Notes

This fork includes several enhancements for macOS compatibility:

### Changes Made
- **Platform-aware CMake configuration** - Automatically detects macOS and uses Homebrew dependencies
- **Custom FFmpeg finder module** - Locates FFmpeg libraries via pkg-config on macOS
- **Fixed Windows-only code** - Wrapped Windows-specific headers and macros in `#ifdef _WIN32`
- **Boost compatibility** - Updated to work with Homebrew's Boost 1.90+ (header-only mode)
- **Proxy SSL bypass** - Automatically disables SSL verification when using a proxy (required for mitmproxy)
- **Client ID support** - Added `udemy_client_id` configuration option
- **Browser fingerprinting headers** - Includes sec-ch-ua headers to help bypass Cloudflare

### Build Artifacts
After building, the executable is located at:
```
build/UdemySaver/UdemySaver
```

Run it from this directory to ensure `settings.ini` and the `www/` folder are properly located.

## Legal
This tool is intended for downloading courses you have legally purchased.
Respect Udemy's Terms of Service and local laws when using it.

## License
This project is licensed under the [MIT License](LICENSE).
