package com.github.stenzek.duckstation;

import android.content.Context;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.TextView;

import java.util.ArrayList;

public class PropertyListAdapter extends BaseAdapter {
    private class Item {
        public String key;
        public String title;
        public String value;

        Item(String key, String title, String value) {
            this.key = key;
            this.title = title;
            this.value = value;
        }
    }

    private Context mContext;
    private ArrayList<Item> mItems = new ArrayList<>();

    public PropertyListAdapter(Context context) {
        mContext = context;
    }

    public Item getItemByKey(String key) {
        for (Item it : mItems) {
            if (it.key.equals(key))
                return it;
        }

        return null;
    }

    public int addItem(String key, String title, String value) {
        if (getItemByKey(key) != null)
            return -1;

        Item it = new Item(key, title, value);
        int position = mItems.size();
        mItems.add(it);
        return position;
    }

    public boolean removeItem(Item item) {
        return mItems.remove(item);
    }

    @Override
    public int getCount() {
        return mItems.size();
    }

    @Override
    public Object getItem(int position) {
        return mItems.get(position);
    }

    @Override
    public long getItemId(int position) {
        return position;
    }

    @Override
    public View getView(int position, View convertView, ViewGroup parent) {
        if (convertView == null) {
            convertView = LayoutInflater.from(mContext)
                    .inflate(R.layout.layout_game_property_entry, parent, false);
        }

        TextView titleView = (TextView) convertView.findViewById(R.id.property_title);
        TextView valueView = (TextView) convertView.findViewById(R.id.property_value);
        Item prop = mItems.get(position);
        titleView.setText(prop.title);
        valueView.setText(prop.value);
        return convertView;
    }
}
