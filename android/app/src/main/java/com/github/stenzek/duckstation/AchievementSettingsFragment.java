package com.github.stenzek.duckstation;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.AsyncTask;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.DialogFragment;
import androidx.preference.Preference;
import androidx.preference.PreferenceFragmentCompat;
import androidx.preference.PreferenceScreen;

import java.net.URLEncoder;
import java.text.DateFormat;
import java.util.Date;
import java.util.Locale;

public class AchievementSettingsFragment extends PreferenceFragmentCompat implements Preference.OnPreferenceClickListener {
    private static final String REGISTER_URL = "http://retroachievements.org/createaccount.php";
    private static final String PROFILE_URL_PREFIX = "https://retroachievements.org/user/";

    private boolean isLoggedIn = false;
    private String username;
    private String loginTokenTime;

    public AchievementSettingsFragment() {
    }

    @Override
    public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
        setPreferencesFromResource(R.xml.achievement_preferences, rootKey);
        updateViews();
    }

    private void updateViews() {
        final SharedPreferences prefs = getPreferenceManager().getSharedPreferences();

        username = prefs.getString("Cheevos/Username", "");
        isLoggedIn = (username != null && !username.isEmpty());
        if (isLoggedIn) {
            try {
                final String loginTokenTimeString = prefs.getString("Cheevos/LoginTimestamp", "");
                final long loginUnixTimestamp = Long.parseLong(loginTokenTimeString);

                // TODO: Extract to a helper function.
                final Date date = new Date(loginUnixTimestamp * 1000);
                final DateFormat format = DateFormat.getDateTimeInstance(DateFormat.DEFAULT, DateFormat.SHORT, Locale.getDefault());
                loginTokenTime = format.format(date);
            } catch (Exception e) {
                loginTokenTime = null;
            }
        }

        final PreferenceScreen preferenceScreen = getPreferenceScreen();

        Preference preference = preferenceScreen.findPreference("Cheevos/ChallengeMode");
        if (preference != null) {
            // toggling this is disabled while it's running to avoid the whole power off thing
            preference.setEnabled(!AndroidHostInterface.getInstance().isEmulationThreadRunning());
        }

        preference = preferenceScreen.findPreference("Cheevos/Login");
        if (preference != null)
        {
            preference.setVisible(!isLoggedIn);
            preference.setOnPreferenceClickListener(this);
        }

        preference = preferenceScreen.findPreference("Cheevos/Register");
        if (preference != null)
        {
            preference.setVisible(!isLoggedIn);
            preference.setOnPreferenceClickListener(this);
        }

        preference = preferenceScreen.findPreference("Cheevos/Logout");
        if (preference != null)
        {
            preference.setVisible(isLoggedIn);
            preference.setOnPreferenceClickListener(this);
        }

        preference = preferenceScreen.findPreference("Cheevos/Username");
        if (preference != null)
        {
            preference.setVisible(isLoggedIn);
            preference.setSummary((username != null) ? username : "");
        }

        preference = preferenceScreen.findPreference("Cheevos/LoginTokenTime");
        if (preference != null)
        {
            preference.setVisible(isLoggedIn);
            preference.setSummary((loginTokenTime != null) ? loginTokenTime : "");
        }

        preference = preferenceScreen.findPreference("Cheevos/ViewProfile");
        if (preference != null)
        {
            preference.setVisible(isLoggedIn);
            preference.setOnPreferenceClickListener(this);
        }
    }

    @Override
    public boolean onPreferenceClick(Preference preference) {
        final String key = preference.getKey();
        if (key == null)
            return false;

        switch (key)
        {
            case "Cheevos/Login":
            {
                handleLogin();
                return true;
            }

            case "Cheevos/Logout":
            {
                handleLogout();
                return true;
            }

            case "Cheevos/Register":
            {
                openUrl(REGISTER_URL);
                return true;
            }

            case "Cheevos/ViewProfile":
            {
                final String profileUrl = getProfileUrl(username);
                if (profileUrl != null)
                    openUrl(profileUrl);

                return true;
            }

            default:
                return false;
        }
    }

    private void openUrl(String url) {
        final Intent browserIntent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
        startActivity(browserIntent);
    }

    private void handleLogin() {
        LoginDialogFragment loginDialog = new LoginDialogFragment(this);
        loginDialog.show(getFragmentManager(), "fragment_achievement_login");
    }

    private void handleLogout() {
        final AlertDialog.Builder builder = new AlertDialog.Builder(getContext());
        builder.setTitle(R.string.settings_achievements_confirm_logout_title);
        builder.setMessage(R.string.settings_achievements_confirm_logout_message);
        builder.setPositiveButton(R.string.settings_achievements_logout, (dialog, which) -> {
                    AndroidHostInterface.getInstance().cheevosLogout();
                    updateViews();
        });
        builder.setNegativeButton(R.string.achievement_settings_login_cancel_button, (dialog, which) -> dialog.dismiss());
        builder.create().show();
    }

    private static String getProfileUrl(String username) {
        try {
            final String encodedUsername = URLEncoder.encode(username, "UTF-8");
            return PROFILE_URL_PREFIX + encodedUsername;
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        }
    }

    public static class LoginDialogFragment extends DialogFragment {
        private AchievementSettingsFragment mParent;

        public LoginDialogFragment(AchievementSettingsFragment parent) {
            mParent = parent;
        }

        @Nullable
        @Override
        public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container, @Nullable Bundle savedInstanceState) {
            return inflater.inflate(R.layout.fragment_achievements_login, container, false);
        }

        @Override
        public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
            super.onViewCreated(view, savedInstanceState);

            ((Button)view.findViewById(R.id.login)).setOnClickListener((View.OnClickListener) v -> doLogin());
            ((Button)view.findViewById(R.id.cancel)).setOnClickListener((View.OnClickListener) v -> dismiss());
        }

        private static class LoginTask extends AsyncTask<Void, Void, Void> {
            private LoginDialogFragment mParent;
            private String mUsername;
            private String mPassword;
            private boolean mResult;

            public LoginTask(LoginDialogFragment parent, String username, String password) {
                mParent = parent;
                mUsername = username;
                mPassword = password;
            }

            @Override
            protected Void doInBackground(Void... voids) {
                final Activity activity = mParent.getActivity();
                if (activity == null)
                    return null;

                mResult = AndroidHostInterface.getInstance().cheevosLogin(mUsername, mPassword);

                activity.runOnUiThread(() -> {
                    if (!mResult) {
                        ((TextView) mParent.getView().findViewById(R.id.error)).setText(R.string.achievement_settings_login_failed);
                        mParent.enableUi(true);
                        return;
                    }

                    mParent.mParent.updateViews();
                    mParent.dismiss();
                });

                return null;
            }
        }

        private void doLogin() {
            final View rootView = getView();
            final String username = ((EditText)rootView.findViewById(R.id.username)).getText().toString();
            final String password = ((EditText)rootView.findViewById(R.id.password)).getText().toString();
            if (username == null || username.length() == 0 || password == null || password.length() == 0)
                return;

            enableUi(false);
            ((TextView)rootView.findViewById(R.id.error)).setText("");
            new LoginDialogFragment.LoginTask(this, username, password).executeOnExecutor(AsyncTask.THREAD_POOL_EXECUTOR);
        }

        private void enableUi(boolean enabled) {
            final View rootView = getView();
            ((EditText)rootView.findViewById(R.id.username)).setEnabled(enabled);
            ((EditText)rootView.findViewById(R.id.password)).setEnabled(enabled);
            ((Button)rootView.findViewById(R.id.login)).setEnabled(enabled);
            ((Button)rootView.findViewById(R.id.cancel)).setEnabled(enabled);
            ((ProgressBar)rootView.findViewById(R.id.progressBar)).setVisibility(enabled ? View.GONE : View.VISIBLE);
        }
    }
}
