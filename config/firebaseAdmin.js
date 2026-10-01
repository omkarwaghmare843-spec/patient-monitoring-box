const { getApps, initializeApp, cert } = require('firebase-admin/app');
const { getAuth } = require('firebase-admin/auth');
const { getFirestore } = require('firebase-admin/firestore');

// FIREBASE_SERVICE_ACCOUNT holds the full service account JSON (from Firebase
// Console > Project Settings > Service Accounts > Generate new private key),
// stored as a single-line env var. Never commit this value - it grants full
// admin access to the Firebase project.
function getAdminApp() {
  if (getApps().length) return getApps()[0];

  const raw = process.env.FIREBASE_SERVICE_ACCOUNT;
  if (!raw) {
    throw new Error('FIREBASE_SERVICE_ACCOUNT environment variable is not set.');
  }

  const serviceAccount = JSON.parse(raw);
  return initializeApp({
    credential: cert(serviceAccount),
  });
}

function adminAuth() {
  return getAuth(getAdminApp());
}

function adminFirestore() {
  return getFirestore(getAdminApp());
}

module.exports = { getAdminApp, adminAuth, adminFirestore };
