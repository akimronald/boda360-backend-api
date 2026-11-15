/**
 * End-to-End Automated GitHub + Render Cloud Provisioner for Boda 360
 *
 * 1. Creates & pushes the divided GitHub repositories using GITHUB_TOKEN:
 *    - boda360-iot-platform        (Full Repo + Render Blueprint)
 *    - boda360-backend-api         (Part 1: Node.js Backend API + WebSocket + MQTT Gateway)
 *    - boda360-customer-portal     (Part 2: Customer Tracking Static Site)
 *    - boda360-manufacturer-portal (Part 3: Manufacturer & OTA Admin Static Site)
 * 2. Provisions (or triggers deployments for) all 3 services on Render.com
 *    via the Render REST API v1 (https://api.render.com/v1) using RENDER_API_KEY.
 */

const { execSync } = require('child_process');
const https = require('https');
const os = require('os');
const path = require('path');
const dotenv = require('dotenv');

const ROOT = path.resolve(__dirname, '..');
dotenv.config({ path: path.join(os.homedir(), '.env') });
dotenv.config({ path: path.join(ROOT, '.env') });

const githubToken = (process.env.GITHUB_TOKEN || '').trim();
const renderApiKey = (process.env.RENDER_API_KEY || '').trim();

if (!githubToken || !renderApiKey) {
  if (!githubToken) console.error('Missing GITHUB_TOKEN in ~/.env or ./.env');
  if (!renderApiKey) console.error('Missing RENDER_API_KEY in ~/.env or ./.env');
  process.exit(1);
}

function jsonRequest(hostname, method, apiPath, token, bodyObj) {
  return new Promise((resolve, reject) => {
    const body = bodyObj ? JSON.stringify(bodyObj) : null;
    const req = https.request(
      {
        hostname,
        path: apiPath,
        method,
        headers: {
          'User-Agent': 'Boda360-Cloud-Deployer',
          'Accept': 'application/json',
          'Authorization': `Bearer ${token}`,
          ...(body ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) } : {})
        }
      },
      (res) => {
        let data = '';
        res.on('data', (chunk) => (data += chunk));
        res.on('end', () => {
          try {
            resolve({ status: res.statusCode, data: data ? JSON.parse(data) : {} });
          } catch (_) {
            resolve({ status: res.statusCode, data: { raw: data } });
          }
        });
      }
    );
    req.on('error', reject);
    if (body) req.write(body);
    req.end();
  });
}

async function getRenderOwnerId() {
  const res = await jsonRequest('api.render.com', 'GET', '/v1/owners?limit=10', renderApiKey);
  if (res.status !== 200 || !Array.isArray(res.data) || res.data.length === 0) {
    throw new Error(`Unable to fetch Render owner account (${res.status}): ${JSON.stringify(res.data)}`);
  }
  const owner = res.data[0].owner || res.data[0];
  console.log(`☁️  Authenticated with Render Owner: ${owner.name || owner.email} (${owner.id})`);
  return owner.id;
}

async function findExistingRenderService(serviceName) {
  const res = await jsonRequest('api.render.com', 'GET', `/v1/services?name=${encodeURIComponent(serviceName)}&limit=5`, renderApiKey);
  if (res.status === 200 && Array.isArray(res.data)) {
    for (const item of res.data) {
      const svc = item.service || item;
      if (svc && svc.name === serviceName) return svc;
    }
  }
  return null;
}

async function ensureRenderWebService(ownerId, githubUser) {
  const serviceName = 'boda360-backend-api';
  const existing = await findExistingRenderService(serviceName);
  if (existing) {
    const url = (existing.serviceDetails && existing.serviceDetails.url) || `https://${serviceName}.onrender.com`;
    console.log(`ℹ️  Render Web Service '${serviceName}' already exists: ${url} — triggering fresh deploy...`);
    await jsonRequest('api.render.com', 'POST', `/v1/services/${existing.id}/deploys`, renderApiKey, { clearCache: 'do_not_clear' });
    return url;
  }

  const payload = {
    type: 'web_service',
    name: serviceName,
    ownerId,
    repo: `https://github.com/${githubUser}/boda360-backend-api`,
    branch: 'main',
    autoDeploy: 'yes',
    envVars: [
      { key: 'NODE_ENV', value: 'production' },
      { key: 'ENABLE_SIMULATOR', value: 'false' },
      { key: 'MQTT_BROKER_HOST', value: 'broker.hivemq.com' },
      { key: 'MQTT_BROKER_PORT', value: '1883' }
    ],
    serviceDetails: {
      runtime: 'node',
      plan: 'free',
      region: 'oregon',
      healthCheckPath: '/healthz',
      envSpecificDetails: {
        buildCommand: 'npm install',
        startCommand: 'npm start'
      }
    }
  };

  const createRes = await jsonRequest('api.render.com', 'POST', '/v1/services', renderApiKey, payload);
  if (createRes.status !== 201 && createRes.status !== 200) {
    throw new Error(`Failed to create Render Web Service '${serviceName}' (${createRes.status}): ${JSON.stringify(createRes.data)}`);
  }

  const svc = createRes.data.service || createRes.data;
  const url = (svc.serviceDetails && svc.serviceDetails.url) || `https://${serviceName}.onrender.com`;
  console.log(`✅ Provisioned Render Backend Web Service: ${url}`);
  return url;
}

async function ensureRenderStaticSite(ownerId, serviceName, repoName, githubUser, backendUrl) {
  const existing = await findExistingRenderService(serviceName);
  if (existing) {
    const url = (existing.serviceDetails && existing.serviceDetails.url) || `https://${serviceName}.onrender.com`;
    console.log(`ℹ️  Render Static Site '${serviceName}' already exists: ${url} — triggering fresh deploy...`);
    await jsonRequest('api.render.com', 'POST', `/v1/services/${existing.id}/deploys`, renderApiKey, { clearCache: 'do_not_clear' });
    return url;
  }

  const payload = {
    type: 'static_site',
    name: serviceName,
    ownerId,
    repo: `https://github.com/${githubUser}/${repoName}`,
    branch: 'main',
    autoDeploy: 'yes',
    envVars: [
      { key: 'API_BASE_URL', value: backendUrl }
    ],
    serviceDetails: {
      buildCommand: '',
      publishPath: '.'
    }
  };

  const createRes = await jsonRequest('api.render.com', 'POST', '/v1/services', renderApiKey, payload);
  if (createRes.status !== 201 && createRes.status !== 200) {
    throw new Error(`Failed to create Render Static Site '${serviceName}' (${createRes.status}): ${JSON.stringify(createRes.data)}`);
  }

  const svc = createRes.data.service || createRes.data;
  const url = (svc.serviceDetails && svc.serviceDetails.url) || `https://${serviceName}.onrender.com`;
  console.log(`✅ Provisioned Render Static Site '${serviceName}': ${url}`);
  return url;
}

async function main() {
  // 1. Verify GitHub User
  const ghUserRes = await jsonRequest('api.github.com', 'GET', '/user', githubToken);
  if (ghUserRes.status !== 200 || !ghUserRes.data.login) {
    throw new Error(`Invalid GITHUB_TOKEN (${ghUserRes.status})`);
  }
  const githubUser = ghUserRes.data.login;

  // 2. Verify Render Owner
  const ownerId = await getRenderOwnerId();

  // 3. Push repositories to GitHub first (with API_BASE_URL set to the Render Backend URL)
  const defaultBackendUrl = `https://boda360-backend-api.onrender.com`;
  execSync('node scripts/push-to-github.js', {
    cwd: ROOT,
    stdio: 'inherit',
    env: { ...process.env, API_BASE_URL: defaultBackendUrl }
  });

  // 4. Provision Backend API Web Service on Render
  const liveBackendUrl = await ensureRenderWebService(ownerId, githubUser);

  // If Render assigned a suffixed URL (e.g. https://boda360-backend-api-xxxx.onrender.com),
  // rebuild and re-push the static portals with the exact live Backend URL
  if (liveBackendUrl && liveBackendUrl !== defaultBackendUrl) {
    console.log(`🔄 Updating static portals to point to live backend URL: ${liveBackendUrl}`);
    execSync('node scripts/push-to-github.js', {
      cwd: ROOT,
      stdio: 'inherit',
      env: { ...process.env, API_BASE_URL: liveBackendUrl }
    });
  }

  // 5. Provision Customer Portal Static Site on Render
  const customerUrl = await ensureRenderStaticSite(
    ownerId,
    'boda360-customer-portal',
    'boda360-customer-portal',
    githubUser,
    liveBackendUrl
  );

  // 6. Provision Manufacturer Portal Static Site on Render
  const manufacturerUrl = await ensureRenderStaticSite(
    ownerId,
    'boda360-manufacturer-portal',
    'boda360-manufacturer-portal',
    githubUser,
    liveBackendUrl
  );

  console.log('\n================================================================');
  console.log('🎉 BODA 360 GITHUB + RENDER DEPLOYMENT COMPLETE!');
  console.log(`📡 Part 1 (Backend API + WS):    ${liveBackendUrl}`);
  console.log(`🏍️  Part 2 (Customer Portal):     ${customerUrl}`);
  console.log(`🏭 Part 3 (Manufacturer Portal): ${manufacturerUrl}`);
  console.log('================================================================');
}

main().catch((err) => {
  console.error('❌ Deployment error:', err.message);
  process.exit(1);
});
