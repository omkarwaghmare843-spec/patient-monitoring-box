import { auth, db, onAuthStateChanged, signOut, doc, getDoc } from './firebase-init.js';

function initials(name) {
  if (!name) return '?';
  return name
    .split(' ')
    .filter(Boolean)
    .slice(0, 2)
    .map((p) => p[0].toUpperCase())
    .join('');
}

export function requireAuth(onReady) {
  onAuthStateChanged(auth, async (user) => {
    if (!user) {
      window.location.href = '/';
      return;
    }

    let profile = { name: user.displayName || user.email, relation: '' };
    try {
      const snap = await getDoc(doc(db, 'users', user.uid));
      if (snap.exists()) profile = { ...profile, ...snap.data() };
    } catch (err) {
      console.warn('Could not load user profile:', err);
    }

    const nameEl = document.getElementById('sidebarName');
    const emailEl = document.getElementById('sidebarEmail');
    const avatarEl = document.getElementById('sidebarAvatar');
    if (nameEl) nameEl.textContent = profile.name || user.email;
    if (emailEl) emailEl.textContent = user.email;
    if (avatarEl) avatarEl.textContent = initials(profile.name || user.email);

    if (typeof onReady === 'function') onReady(user, profile);
  });
}

const logoutBtn = document.getElementById('logoutBtn');
if (logoutBtn) {
  logoutBtn.addEventListener('click', async () => {
    await signOut(auth);
    window.location.href = '/';
  });
}

// Mobile nav toggle
const navToggle = document.getElementById('navToggle');
if (navToggle) {
  navToggle.addEventListener('click', () => {
    document.querySelector('.app-shell').classList.toggle('nav-open');
  });
  document.addEventListener('click', (e) => {
    const shell = document.querySelector('.app-shell');
    if (!shell || !shell.classList.contains('nav-open')) return;
    if (!e.target.closest('.sidebar') && !e.target.closest('#navToggle')) {
      shell.classList.remove('nav-open');
    }
  });
}

// Highlight active nav link based on current path
document.querySelectorAll('.nav-link').forEach((link) => {
  if (link.getAttribute('href') === window.location.pathname) {
    link.classList.add('active');
  }
});

export function toast(message, type = 'default') {
  let container = document.querySelector('.toast-container');
  if (!container) {
    container = document.createElement('div');
    container.className = 'toast-container';
    document.body.appendChild(container);
  }
  const el = document.createElement('div');
  el.className = `toast toast-${type}`;
  el.textContent = message;
  container.appendChild(el);
  setTimeout(() => el.remove(), 3800);
}
