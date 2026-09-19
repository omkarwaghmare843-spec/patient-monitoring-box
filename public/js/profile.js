import { requireAuth, toast } from './app-shell.js';
import { db, doc, setDoc, updateProfile, serverTimestamp } from './firebase-init.js';

const form = document.getElementById('profileForm');
const nameInput = document.getElementById('profileName');
const relationInput = document.getElementById('profileRelation');
const emailInput = document.getElementById('profileEmail');
const saveBtn = document.getElementById('saveProfileBtn');
const formError = document.getElementById('formError');
const formErrorText = document.getElementById('formErrorText');
const formSuccess = document.getElementById('formSuccess');

let currentUser = null;

requireAuth((user, profile) => {
  currentUser = user;
  nameInput.value = profile.name || user.displayName || '';
  relationInput.value = profile.relation || 'Self';
  emailInput.value = user.email;
});

form.addEventListener('submit', async (e) => {
  e.preventDefault();
  formError.classList.remove('active');
  formSuccess.classList.remove('active');

  saveBtn.disabled = true;
  saveBtn.innerHTML = '<span class="spinner dark"></span> Saving…';

  try {
    await setDoc(
      doc(db, 'users', currentUser.uid),
      { name: nameInput.value.trim(), relation: relationInput.value, updatedAt: serverTimestamp() },
      { merge: true }
    );
    await updateProfile(currentUser, { displayName: nameInput.value.trim() });

    document.getElementById('sidebarName').textContent = nameInput.value.trim();
    toast('Profile updated.', 'success');
    formSuccess.querySelector('#formSuccessText').textContent = 'Profile updated successfully.';
    formSuccess.classList.add('active');
  } catch (err) {
    console.error(err);
    formErrorText.textContent = 'Failed to update profile. Please try again.';
    formError.classList.add('active');
  } finally {
    saveBtn.disabled = false;
    saveBtn.textContent = 'Save Changes';
  }
});
