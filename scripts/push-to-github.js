/**
 * Automatically creates the Boda 360 GitHub repositories and pushes:
 *   1. boda360-iot-platform        (Full Repository + Render 1-Click Blueprint render.yaml)
 *   2. boda360-backend-api         (Part 1: Node.js Backend API, WebSocket & MQTT Gateway)
 *   3. boda360-customer-portal     (Part 2: Customer & Public Tracking Static Portal)
 *   4. boda360-manufacturer-portal (Part 3: Manufacturer Batch & Dual-MCU OTA Static Portal)
 *
 * Reads GITHUB_TOKEN from ~/.env or ./.env
 */

const { execSync } = require('child_process');
const https = require('https');
const fs = require('fs');
const os = require('os');
const path = require('path');
const dotenv = require('dotenv');

const ROOT = path.resolve(__dirname, '..');
dotenv.config({ path: path.join(os.homedir(), '.env') });
dotenv.config({ path: path.join(ROOT, '.env') });

const token = (process.env.GITHUB_TOKEN || '').trim();
if (!token) {
  console.error('Missing GITHUB_TOKEN in ~/.env or ./.env');
  process.exit(1);
}

function githubRequest(method, apiPath, bodyObj) {
  return new Promise((resolve, reject) => {
    const body = bodyObj ? JSON.stringify(bodyObj) : null;
    const req = https.request(
      {
        hostname: 'api.github.com',
        path: apiPath,
        method,
        headers: {
          'User-Agent': 'Boda360-Deployer',
          'Accept': 'application/vnd.github+json',
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

async function ensureRepo(username, repoName, description) {
  const checkRes = await githubRequest('GET', `/repos/${username}/${repoName}`);
  if (checkRes.status === 200) {
    console.log(`ℹ️  GitHub repository exists: https://github.com/${username}/${repoName}`);
  } else {
    const res = await githubRequest('POST', '/user/repos', {
      name: repoName,
      description,
      private: false,
      auto_init: false
    });

    if (res.status === 201) {
      console.log(`✅ Created GitHub repository: ${res.data.html_url}`);
    } else if (res.status === 422) {
      console.log(`ℹ️  GitHub repository exists: https://github.com/${username}/${repoName}`);
    } else {
      throw new Error(`GitHub API (${res.status}): ${JSON.stringify(res.data)}`);
    }
  }

  return {
    htmlUrl: `https://github.com/${username}/${repoName}`,
    authUrl: `https://x-access-token:${token}@github.com/${username}/${repoName}.git`,
    cleanUrl: `https://github.com/${username}/${repoName}.git`
  };
}

function pushDirectoryToRepo(dirPath, repoInfo, remoteName = 'origin') {
  if (!fs.existsSync(path.join(dirPath, '.git'))) {
    execSync('git init -b main', { cwd: dirPath, stdio: 'pipe' });
  }
  execSync('git add .', { cwd: dirPath, stdio: 'pipe' });
  try {
    execSync('git commit -m "Deploy Boda 360 IoT Platform"', { cwd: dirPath, stdio: 'pipe' });
  } catch (_) {}

  try {
    execSync(`git remote remove ${remoteName}`, { cwd: dirPath, stdio: 'ignore' });
  } catch (_) {}

  execSync(`git remote add ${remoteName} "${repoInfo.authUrl}"`, { cwd: dirPath, stdio: 'pipe' });
  execSync(`git push -u ${remoteName} main --force`, { cwd: dirPath, stdio: 'inherit' });
  // Reset remote URL to clean public URL so token is never stored in .git/config
  execSync(`git remote set-url ${remoteName} "${repoInfo.cleanUrl}"`, { cwd: dirPath, stdio: 'pipe' });
  console.log(`🚀 Pushed to ${repoInfo.htmlUrl}`);
}

async function main() {
  const userRes = await githubRequest('GET', '/user');
  if (userRes.status !== 200 || !userRes.data.login) {
    throw new Error(`Failed to authenticate GITHUB_TOKEN (${userRes.status})`);
  }
  const username = userRes.data.login;
  console.log(`🔑 Authenticated as GitHub user: ${username}`);

  // Build fresh static bundles
  execSync('node scripts/prepare-render-static.js', { cwd: ROOT, stdio: 'inherit' });

  // 1. Unified Full-Stack + Render Blueprint Repo
  const mainRepo = await ensureRepo(
    username,
    'boda360-iot-platform',
    'Boda 360 — IoT Real-Time Motorcycle Tracking, Remote Immobilization & Dual-MCU Internet OTA Platform (Render Blueprint)'
  );
  pushDirectoryToRepo(ROOT, mainRepo, 'origin');

  // 2. Divided Part 1: Backend API Repo
  const apiRepo = await ensureRepo(
    username,
    'boda360-backend-api',
    'Boda 360 — Part 1: Central Node.js Backend API, WebSocket Telemetry & HiveMQ Cellular MQTT Gateway'
  );
  pushDirectoryToRepo(ROOT, apiRepo, 'backend-repo');

  // 3. Divided Part 2: Customer Portal Static Repo
  const custRepo = await ensureRepo(
    username,
    'boda360-customer-portal',
    'Boda 360 — Part 2: Customer & Rider Real-Time GPS Tracking and Remote Immobilization Portal'
  );
  pushDirectoryToRepo(path.join(ROOT, 'dist', 'customer'), custRepo, 'origin');

  // 4. Divided Part 3: Manufacturer Portal Static Repo
  const mfgRepo = await ensureRepo(
    username,
    'boda360-manufacturer-portal',
    'Boda 360 — Part 3: Manufacturer Hardware Batch Provisioning & Dual-Board Internet OTA Admin Portal'
  );
  pushDirectoryToRepo(path.join(ROOT, 'dist', 'manufacturer'), mfgRepo, 'origin');
}

main().catch((err) => {
  console.error('❌ Push failed:', err.message);
  process.exit(1);
});
