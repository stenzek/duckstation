package com.github.stenzek.duckstation;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Resources;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.BaseAdapter;
import android.widget.Spinner;
import android.widget.TextView;

import androidx.annotation.Nullable;
import androidx.preference.Preference;
import androidx.preference.PreferenceDataStore;
import androidx.preference.PreferenceManager;
import androidx.preference.PreferenceViewHolder;

public class RatioPreference extends Preference {
    private String mNumeratorKey;
    private String mDenominatorKey;

    private int mNumeratorValue = 1;
    private int mDenominatorValue = 1;

    private int mMinimumNumerator = 1;
    private int mMaximumNumerator = 50;
    private int mDefaultNumerator = 1;
    private int mMinimumDenominator = 1;
    private int mMaximumDenominator = 50;
    private int mDefaultDenominator = 1;

    private void initAttributes(AttributeSet attrs) {
        for (int i = 0; i < attrs.getAttributeCount(); i++) {
            final String key = attrs.getAttributeName(i);
            if (key.equals("numeratorKey")) {
                mNumeratorKey = attrs.getAttributeValue(i);
            } else if (key.equals("minimumNumerator")) {
                mMinimumNumerator = attrs.getAttributeIntValue(i, 1);
            } else if (key.equals("maximumNumerator")) {
                mMaximumNumerator = attrs.getAttributeIntValue(i, 1);
            } else if (key.equals("defaultNumerator")) {
                mDefaultNumerator = attrs.getAttributeIntValue(i, 1);
            } else if(key.equals("denominatorKey")) {
                mDenominatorKey = attrs.getAttributeValue(i);
            } else if (key.equals("minimumDenominator")) {
                mMinimumDenominator = attrs.getAttributeIntValue(i, 1);
            } else if (key.equals("maximumDenominator")) {
                mMaximumDenominator = attrs.getAttributeIntValue(i, 1);
            } else if (key.equals("defaultDenominator")) {
                mDefaultDenominator = attrs.getAttributeIntValue(i, 1);
            }
        }
    }

    public RatioPreference(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        setWidgetLayoutResource(R.layout.layout_ratio_preference);
        initAttributes(attrs);
    }

    public RatioPreference(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        setWidgetLayoutResource(R.layout.layout_ratio_preference);
        initAttributes(attrs);
    }

    public RatioPreference(Context context, AttributeSet attrs) {
        super(context, attrs);
        setWidgetLayoutResource(R.layout.layout_ratio_preference);
        initAttributes(attrs);
    }

    public RatioPreference(Context context) {
        super(context);
        setWidgetLayoutResource(R.layout.layout_ratio_preference);
    }

    private void persistValues() {
        final PreferenceDataStore dataStore = getPreferenceDataStore();
        if (dataStore != null) {
            if (mNumeratorKey != null)
                dataStore.putInt(mNumeratorKey, mNumeratorValue);
            if (mDenominatorKey != null)
                dataStore.putInt(mDenominatorKey, mDenominatorValue);
        } else {
            SharedPreferences.Editor editor = getPreferenceManager().getSharedPreferences().edit();
            if (mNumeratorKey != null)
                editor.putInt(mNumeratorKey, mNumeratorValue);
            if (mDenominatorKey != null)
                editor.putInt(mDenominatorKey, mDenominatorValue);
            editor.commit();
        }
    }

    @Override
    protected void onAttachedToHierarchy(PreferenceManager preferenceManager) {
        super.onAttachedToHierarchy(preferenceManager);
        setInitialValue();
    }

    private void setInitialValue() {
        final PreferenceDataStore dataStore = getPreferenceDataStore();
        if (dataStore != null) {
            if (mNumeratorKey != null)
                mNumeratorValue = dataStore.getInt(mNumeratorKey, mDefaultNumerator);
            if (mDenominatorKey != null)
                mDenominatorValue = dataStore.getInt(mDenominatorKey, mDefaultDenominator);
        } else {
            final SharedPreferences pm = getPreferenceManager().getSharedPreferences();
            if (mNumeratorKey != null)
                mNumeratorValue = pm.getInt(mNumeratorKey, mDefaultNumerator);
            if (mDenominatorKey != null)
                mDenominatorValue = pm.getInt(mDenominatorKey, mDefaultDenominator);
        }
    }

    private static BaseAdapter generateDropdownItems(int min, int max) {
        return new BaseAdapter() {
            @Override
            public int getCount() {
                return (max - min) + 1;
            }

            @Override
            public Object getItem(int position) {
                return Integer.toString(min + position);
            }

            @Override
            public long getItemId(int position) {
                return position;
            }

            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                TextView view;
                if (convertView != null) {
                    view = (TextView) convertView;
                } else {
                    view = new TextView(parent.getContext());

                    Resources r = parent.getResources();
                    float px = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 10.0f, r.getDisplayMetrics());
                    view.setPadding((int) px, (int) px, (int) px, (int) px);
                }

                view.setText(Integer.toString(min + position));
                return view;
            }
        };
    }

    @Override
    public void onBindViewHolder(PreferenceViewHolder holder) {
        super.onBindViewHolder(holder);
        holder.itemView.setClickable(false);

        Spinner numeratorSpinner = (Spinner) holder.findViewById(R.id.numerator);
        numeratorSpinner.setAdapter(generateDropdownItems(mMinimumNumerator, mMaximumNumerator));
        numeratorSpinner.setSelection(mNumeratorValue - mMinimumNumerator);
        numeratorSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                final int newValue = position + mMinimumNumerator;
                if (newValue != mNumeratorValue) {
                    mNumeratorValue = newValue;
                    persistValues();
                }
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
        Spinner denominatorSpinner = (Spinner) holder.findViewById(R.id.denominator);
        denominatorSpinner.setAdapter(generateDropdownItems(mMinimumDenominator, mMaximumDenominator));
        denominatorSpinner.setSelection(mDenominatorValue - mMinimumDenominator);
        denominatorSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                final int newValue = position + mMinimumDenominator;
                if (newValue != mDenominatorValue) {
                    mDenominatorValue = newValue;
                    persistValues();
                }
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
    }


}
