/**
 * JWT Authentication & RBAC Middleware
 * Author: Akim Ronald David (ISBAT University)
 */

const jwt = require('jsonwebtoken');
require('dotenv').config();

const JWT_SECRET = process.env.JWT_SECRET || 'boda_secur1ty_jwt_sup3r_secr3t_k4mp4la_2026';

function verifyToken(req, res, next) {
  const authHeader = req.headers['authorization'];
  let token = null;

  if (authHeader && authHeader.startsWith('Bearer ')) {
    token = authHeader.split(' ')[1];
  } else if (req.query && req.query.token) {
    token = req.query.token;
  }

  if (!token) {
    return res.status(401).json({
      success: false,
      message: 'Access Denied: Missing JSON Web Token in Authorization header.'
    });
  }

  try {
    const verified = jwt.verify(token, JWT_SECRET);
    req.user = verified;
    next();
  } catch (err) {
    return res.status(403).json({
      success: false,
      message: 'Access Denied: Invalid or expired authentication token.'
    });
  }
}

function requireRole(...roles) {
  return (req, res, next) => {
    if (!req.user || !roles.includes(req.user.role)) {
      return res.status(403).json({
        success: false,
        message: `Forbidden: This operation requires one of [${roles.join(', ')}] roles.`
      });
    }
    next();
  };
}

module.exports = {
  verifyToken,
  requireRole,
  JWT_SECRET
};
