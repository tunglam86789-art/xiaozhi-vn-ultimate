#ifndef WEATHER_CONFIG_H
#define WEATHER_CONFIG_H

// Enable/Disable weather feature
#ifndef CONFIG_ENABLE_WEATHER_FEATURE
#define CONFIG_ENABLE_WEATHER_FEATURE 1
#endif

// Weather update interval (in milliseconds)
#ifndef WEATHER_UPDATE_INTERVAL_MS
#define WEATHER_UPDATE_INTERVAL_MS (30 * 60 * 1000)  // 30 minutes
#endif

// Default OpenWeatherMap API key
#ifndef OPEN_WEATHERMAP_API_KEY_DEFAULT
#define OPEN_WEATHERMAP_API_KEY_DEFAULT "ae8d3c2fda691593ce3e84472ef25784"
#endif

// Weather API endpoints
#define WEATHER_API_ENDPOINT "https://api.openweathermap.org/data/2.5/weather"
#define IP_LOCATION_API_ENDPOINT "https://ipwho.is"

// HTTP timeout settings
#define WEATHER_HTTP_TIMEOUT_MS 10000

#endif // WEATHER_CONFIG_H
