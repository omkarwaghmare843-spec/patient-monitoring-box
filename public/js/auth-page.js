import {
  auth,
  db,
  onAuthStateChanged,
  createUserWithEmailAndPassword,
  signInWithEmailAndPassword,
  updateProfile,
  doc,
  setDoc,
  getDoc,
  serverTimestamp,
} from './firebase-init.js';

const tabBtns = document.querySelectorAll('.tab-btn');
const panels = {
  signin: document.getElementById('panel-signin'),
  signup: document.getElementById('panel-signup'),
};

tabBtns.forEach((btn) => {
  btn.addEventListener('click', () => {
    tabBtns.forEach((b) => b.classList.remove('active'));
    Object.values(panels).forEach((p) => p.classList.remove('active'));
    btn.classList.add('active');
    panels[btn.dataset.tab].classList.add('active');
    hideMessages();
  });
});

const formError = document.getElementById('formError');
const formErrorText = document.getElementById('formErrorText');
const formSuccess = document.getElementById('formSuccess');
const formSuccessText = document.getElementById('formSuccessText');

function showError(message) {
  formSuccess.classList.remove('active');
  formErrorText.textContent = message;
  formError.classList.add('active');
}

function showSuccess(message) {
  formError.classList.remove('active');
  formSuccessText.textContent = message;
  formSuccess.classList.add('active');
}

function hideMessages() {
  formError.classList.remove('active');
  formSuccess.classList.remove('active');
}

function setLoading(button, loading, label) {
  button.disabled = loading;
  button.innerHTML = loading
    ? '<span class="spinner"></span> Please wait…'
    : label;
}

function friendlyAuthError(err) {
  const code = err && err.code ? err.code : '';
  const map = {
    'auth/invalid-email': 'That email address looks invalid.',
    'auth/user-not-found': 'No account found with that email.',
    'auth/wrong-password': 'Incorrect password. Please try again.',
    'auth/invalid-credential': 'Incorrect email or password.',
    'auth/email-already-in-use': 'An account with this email already exists.',
    'auth/weak-password': 'Password should be at least 6 characters.',
    'auth/too-many-requests': 'Too many attempts. Please wait a moment and try again.',
    'auth/network-request-failed': 'Network error. Check your connection and try again.',
  };
  return map[code] || (err && err.message) || 'Something went wrong. Please try again.';
}

// Redirect to dashboard if already signed in.
onAuthStateChanged(auth, (user) => {
  if (user) window.location.href = '/dashboard';
});

// ----- Sign In -----
const signinForm = document.getElementById('signinForm');
signinForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  hideMessages();
  const btn = document.getElementById('signinBtn');
  const email = document.getElementById('signinEmail').value.trim();
  const password = document.getElementById('signinPassword').value;

  setLoading(btn, true);
  try {
    await signInWithEmailAndPassword(auth, email, password);
    window.location.href = '/dashboard';
  } catch (err) {
    showError(friendlyAuthError(err));
    setLoading(btn, false, 'Sign In');
  }
});

// ----- Sign Up -----
const signupForm = document.getElementById('signupForm');
signupForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  hideMessages();
  const btn = document.getElementById('signupBtn');
  const name = document.getElementById('signupName').value.trim();
  const relation = document.getElementById('signupRelation').value;
  const email = document.getElementById('signupEmail').value.trim();
  const password = document.getElementById('signupPassword').value;

  setLoading(btn, true);
  try {
    const cred = await createUserWithEmailAndPassword(auth, email, password);
    await updateProfile(cred.user, { displayName: name });

    await setDoc(doc(db, 'users', cred.user.uid), {
      name,
      relation,
      email,
      createdAt: serverTimestamp(),
    });

    window.location.href = '/dashboard';
  } catch (err) {
    showError(friendlyAuthError(err));
    setLoading(btn, false, 'Create Account');
  }
});
