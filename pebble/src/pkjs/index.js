var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

Pebble.addEventListener('showConfiguration', function() {
  Pebble.openURL(clay.generateUrl());
});

function valueOf(setting) {
  return typeof setting === 'object' ? setting.value : setting;
}

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) return;

  var settings = clay.getSettings(e.response, false);
  var dict = {};

  if (settings.BG_HEX !== undefined) {
    dict.BG_HEX = valueOf(settings.BG_HEX);
  }
  if (settings.FACE_HEX !== undefined) {
    dict.FACE_HEX = valueOf(settings.FACE_HEX);
  }
  if (settings.HAND_HEX !== undefined) {
    dict.HAND_HEX = valueOf(settings.HAND_HEX);
  }
  if (settings.DIAL_STYLE !== undefined) {
    dict.DIAL_STYLE = parseInt(valueOf(settings.DIAL_STYLE), 10);
  }
  if (settings.FACE_CLEAR !== undefined) {
    dict.FACE_CLEAR = valueOf(settings.FACE_CLEAR) ? 1 : 0;
  }
  if (settings.THICK_HAND !== undefined) {
    dict.THICK_HAND = valueOf(settings.THICK_HAND) ? 1 : 0;
  }

  if (settings.COMP_MODE !== undefined) {
    dict.COMP_MODE = parseInt(valueOf(settings.COMP_MODE), 10);
  }
  if (settings.TEMP_UNIT !== undefined) {
    dict.TEMP_UNIT = parseInt(valueOf(settings.TEMP_UNIT), 10);
  }

  Pebble.sendAppMessage(dict);
});

// ---- Weather (Open-Meteo, no API key) ----
// Sends the current temperature to the watch as tenths of a degree Celsius;
// the watch converts to the unit chosen in settings.
var WEATHER_REFRESH_MS = 30 * 60 * 1000;

function fetchWeather() {
  navigator.geolocation.getCurrentPosition(function(pos) {
    var url = 'https://api.open-meteo.com/v1/forecast?latitude=' + pos.coords.latitude +
      '&longitude=' + pos.coords.longitude + '&current=temperature_2m&temperature_unit=celsius';
    var req = new XMLHttpRequest();
    req.onload = function() {
      try {
        var c = JSON.parse(req.responseText).current.temperature_2m;
        Pebble.sendAppMessage({ TEMP_VALUE: Math.round(c * 10) });
      } catch (err) {
        console.log('Weather parse error: ' + err);
      }
    };
    req.onerror = function() { console.log('Weather request failed'); };
    req.open('GET', url);
    req.send();
  }, function(err) {
    console.log('Location error: ' + err.message);
  }, { timeout: 15000, maximumAge: 10 * 60 * 1000 });
}

Pebble.addEventListener('ready', function() {
  fetchWeather();
  setInterval(fetchWeather, WEATHER_REFRESH_MS);
});

Pebble.addEventListener('appmessage', function(e) {
  if (e.payload && e.payload.WEATHER_REQUEST !== undefined) {
    fetchWeather();
  }
});
