require('dotenv').config();
const express = require('express');
const path = require('path');
const { getFirebaseConfig } = require('./config/firebaseConfig');

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

app.use((req, res) => {
  res.status(404).render('404', { title: 'Page Not Found' });
});

if (require.main === module) {
  app.listen(PORT, () => {
    console.log(`Patient monitoring UI running at http://localhost:${PORT}`);
  });
}

module.exports = app;
