require('dotenv').config();
const express = require('express');
const path = require('path');
const { getFirebaseConfig } = require('./config/firebaseConfig');
const { adminAuth, adminFirestore } = require('./config/firebaseAdmin');
const { analyzeCheckup } = require('./config/gemini');

const app = express();
const PORT = process.env.PORT || 3000;

app.set('view engine', 'ejs');
app.set('views', path.join(__dirname, 'views'));

app.use(express.static(path.join(__dirname, 'public')));
app.use(express.json());

// Firebase config is loaded client-side; every page needs it, so make it
// available to all views without repeating this in each route.
app.use((req, res, next) => {
  res.locals.firebaseConfig = getFirebaseConfig();
  next();
});

app.get('/', (req, res) => {
  res.render('login', { title: 'Sign In' });
});

app.get('/dashboard', (req, res) => {
  res.render('dashboard', { title: 'Checkup Dashboard' });
});

app.get('/history', (req, res) => {
  res.render('history', { title: 'Checkup History' });
});

app.get('/profile', (req, res) => {
  res.render('profile', { title: 'My Profile' });
});

// Verifies the Firebase ID token sent by the browser and attaches the
// decoded uid to the request. Keeps the Gemini API key and Firestore admin
// writes server-side only - the browser never calls Gemini directly.
async function requireFirebaseAuth(req, res, next) {
  try {
    const header = req.headers.authorization || '';
    const match = header.match(/^Bearer (.+)$/);
    if (!match) {
      return res.status(401).json({ error: 'Missing bearer token.' });
    }
    const decoded = await adminAuth().verifyIdToken(match[1]);
    req.uid = decoded.uid;
    next();
  } catch (err) {
    console.error('Auth verification failed:', err.message);
    res.status(401).json({ error: 'Invalid or expired session.' });
  }
}

app.post('/api/analyze-checkup', requireFirebaseAuth, async (req, res) => {
  const { checkupId } = req.body || {};
  if (!checkupId || typeof checkupId !== 'string') {
    return res.status(400).json({ error: 'checkupId is required.' });
  }

  const db = adminFirestore();
  const checkupRef = db.collection('users').doc(req.uid).collection('checkups').doc(checkupId);

  try {
    const snap = await checkupRef.get();
    if (!snap.exists) {
      return res.status(404).json({ error: 'Checkup not found.' });
    }

    await checkupRef.update({ aiStatus: 'pending' });

    const vitals = snap.data();
    const insight = await analyzeCheckup(vitals);

    await checkupRef.update({ aiInsight: insight, aiStatus: 'done' });
    res.json({ insight });
  } catch (err) {
    console.error('AI analysis failed:', err.message);
    await checkupRef.update({ aiStatus: 'error' }).catch(() => {});
    res.status(500).json({ error: 'AI analysis failed.' });
  }
});

app.use((req, res) => {
  res.status(404).render('404', { title: 'Page Not Found' });
});

if (require.main === module) {
  app.listen(PORT, () => {
    console.log(`Patient monitoring UI running at http://localhost:${PORT}`);
  });
}

module.exports = app;
