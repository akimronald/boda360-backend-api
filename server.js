/**
 * Root entry-point wrapper — delegates directly to backend/server.js
 * so both `node server.js` and `node backend/server.js` (`npm start`)
 * run the unified Central Command & Telematics Gateway with `/healthz`.
 */
const backendServer = require('./backend/server');
if (require.main === module && typeof backendServer.start === 'function') {
  backendServer.start();
}
module.exports = backendServer;
