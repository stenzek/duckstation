package com.github.stenzek.duckstation;

import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Bundle;

import com.google.android.material.tabs.TabLayout;
import com.google.android.material.tabs.TabLayoutMediator;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.ActionBar;
import androidx.appcompat.widget.Toolbar;
import androidx.fragment.app.Fragment;
import androidx.recyclerview.widget.DividerItemDecoration;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import androidx.appcompat.app.AppCompatActivity;
import androidx.viewpager2.adapter.FragmentStateAdapter;
import androidx.viewpager2.widget.ViewPager2;

import android.view.LayoutInflater;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;

public class MemoryCardEditorActivity extends AppCompatActivity {
    public static final int REQUEST_IMPORT_CARD = 1;

    private final ArrayList<MemoryCardImage> cards = new ArrayList<>();
    private CollectionAdapter adapter;
    private ViewPager2 viewPager;
    private TabLayout tabLayout;
    private TabLayoutMediator tabLayoutMediator;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_memory_card_editor);

        Toolbar toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);

        ActionBar actionBar = getSupportActionBar();
        if (actionBar != null) {
            actionBar.setDisplayHomeAsUpEnabled(true);
        }

        adapter = new CollectionAdapter(this);
        viewPager = findViewById(R.id.view_pager);
        viewPager.setAdapter(adapter);

        tabLayout = findViewById(R.id.tab_layout);
        tabLayoutMediator = new TabLayoutMediator(tabLayout, viewPager, adapter.getTabConfigurationStrategy());
        tabLayoutMediator.attach();

        findViewById(R.id.open_card).setOnClickListener((v) -> openCard());
        findViewById(R.id.close_card).setOnClickListener((v) -> closeCard());
    }

    @Override
    protected void onSaveInstanceState(@NonNull Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.remove("android:support:fragments");
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        getMenuInflater().inflate(R.menu.menu_memory_card_editor, menu);

        final boolean hasCurrentCard = (getCurrentCard() != null);
        menu.findItem(R.id.action_delete_card).setEnabled(hasCurrentCard);
        menu.findItem(R.id.action_format_card).setEnabled(hasCurrentCard);
        menu.findItem(R.id.action_import_card).setEnabled(hasCurrentCard);

        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
       switch (item.getItemId()) {
            case android.R.id.home: {
                onBackPressed();
                return true;
            }

            case R.id.action_import_card: {
                importCard();
                return true;
            }

            case R.id.action_delete_card: {
                deleteCard();
                return true;
            }

            case R.id.action_format_card: {
                formatCard();
                return true;
            }

           default: {
               return super.onOptionsItemSelected(item);
           }
        }
    }

    private void openCard() {
        final Uri[] uris = MemoryCardImage.getCardUris(this);
        if (uris == null) {
            displayMessage(getString(R.string.memory_card_editor_no_cards_found));
            return;
        }

        final String[] uriTitles = new String[uris.length];
        for (int i = 0; i < uris.length; i++)
            uriTitles[i] = MemoryCardImage.getTitleForUri(uris[i]);

        final AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setTitle(R.string.memory_card_editor_select_card);
        builder.setItems(uriTitles, (dialog, which) -> {
            final Uri uri = uris[which];
            for (int i = 0; i < cards.size(); i++) {
                if (cards.get(i).getUri().equals(uri)) {
                    displayError(getString(R.string.memory_card_editor_card_already_open));
                    tabLayout.getTabAt(i).select();
                    return;
                }
            }

            final MemoryCardImage card = MemoryCardImage.open(MemoryCardEditorActivity.this, uri);
            if (card == null) {
                displayError(getString(R.string.memory_card_editor_failed_to_open_card));
                return;
            }

            cards.add(card);
            refreshView(card);
        });
        builder.create().show();
    }

    private void closeCard() {
        final int index = tabLayout.getSelectedTabPosition();
        if (index < 0)
            return;

        cards.remove(index);
        refreshView(index);
    }

    private void displayMessage(String message) {
        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
    }

    private void displayError(String message) {
        final AlertDialog.Builder errorBuilder = new AlertDialog.Builder(this);
        errorBuilder.setTitle(R.string.memory_card_editor_error);
        errorBuilder.setMessage(message);
        errorBuilder.setPositiveButton(R.string.main_activity_ok, (dialog1, which1) -> dialog1.dismiss());
        errorBuilder.create().show();
    }

    private void copySave(MemoryCardImage sourceCard, MemoryCardFileInfo sourceFile) {
        if (cards.size() < 2) {
            displayError(getString(R.string.memory_card_editor_must_have_at_least_two_cards_to_copy));
            return;
        }

        if (cards.indexOf(sourceCard) < 0) {
            // this shouldn't happen..
            return;
        }

        final MemoryCardImage[] destinationCards = new MemoryCardImage[cards.size() - 1];
        final String[] cardTitles = new String[cards.size() - 1];
        for (int i = 0, d = 0; i < cards.size(); i++) {
            if (cards.get(i) == sourceCard)
                continue;

            destinationCards[d] = cards.get(i);
            cardTitles[d] = cards.get(i).getTitle();
            d++;
        }

        final AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setTitle(getString(R.string.memory_card_editor_copy_save_to, sourceFile.getTitle()));
        builder.setItems(cardTitles, (dialog, which) -> {
            dialog.dismiss();

            final MemoryCardImage destinationCard = destinationCards[which];

            byte[] data = null;
            if (destinationCard.getFreeBlocks() < sourceFile.getNumBlocks()) {
                displayError(getString(R.string.memory_card_editor_copy_insufficient_blocks, sourceFile.getNumBlocks(),
                        destinationCard.getFreeBlocks()));
            } else if (destinationCard.hasFile(sourceFile.getFilename())) {
                displayError(getString(R.string.memory_card_editor_copy_already_exists, sourceFile.getFilename()));
            } else if ((data = sourceCard.readFile(sourceFile.getFilename())) == null) {
                displayError(getString(R.string.memory_card_editor_copy_read_failed, sourceFile.getFilename()));
            } else if (!destinationCard.writeFile(sourceFile.getFilename(), data)) {
                displayMessage(getString(R.string.memory_card_editor_copy_write_failed, sourceFile.getFilename()));
            } else {
                displayMessage(getString(R.string.memory_card_editor_copy_success, sourceFile.getFilename(),
                        destinationCard.getTitle()));
                refreshView(destinationCard);
            }
        });
        builder.create().show();
    }

    private void deleteSave(MemoryCardImage card, MemoryCardFileInfo file) {
        final AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setMessage(getString(R.string.memory_card_editor_delete_confirm, file.getFilename()));
        builder.setPositiveButton(R.string.main_activity_yes, (dialog, which) -> {
            if (card.deleteFile(file.getFilename())) {
                displayMessage(getString(R.string.memory_card_editor_delete_success, file.getFilename()));
                refreshView(card);
            } else {
                displayError(getString(R.string.memory_card_editor_delete_failed, file.getFilename()));
            }
        });
        builder.setNegativeButton(R.string.main_activity_no, (dialog, which) -> dialog.dismiss());
        builder.create().show();
    }

    private void refreshView(int newSelection) {
        final int oldPos = viewPager.getCurrentItem();
        tabLayoutMediator.detach();
        viewPager.setAdapter(null);
        viewPager.setAdapter(adapter);
        tabLayoutMediator.attach();

        if (cards.isEmpty())
            return;

        if (newSelection < 0 || newSelection >= tabLayout.getTabCount()) {
            if (oldPos < cards.size())
                tabLayout.getTabAt(oldPos).select();
            else
                tabLayout.getTabAt(cards.size() - 1).select();
        } else {
            tabLayout.getTabAt(newSelection).select();
        }
    }

    private void refreshView(MemoryCardImage newSelectedCard) {
        if (newSelectedCard == null)
            refreshView(-1);
        else
            refreshView(cards.indexOf(newSelectedCard));

        invalidateOptionsMenu();
    }

    private MemoryCardImage getCurrentCard() {
        final int index = tabLayout.getSelectedTabPosition();
        if (index < 0 || index >= cards.size())
            return null;

        return cards.get(index);
    }

    private void importCard() {
        if (getCurrentCard() == null) {
            displayMessage(getString(R.string.memory_card_editor_no_card_selected));
            return;
        }

        Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
        intent.setType("*/*");
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        startActivityForResult(Intent.createChooser(intent, getString(R.string.main_activity_choose_disc_image)), REQUEST_IMPORT_CARD);
    }

    private void importCard(Uri uri) {
        final MemoryCardImage card = getCurrentCard();
        if (card == null)
            return;

        final byte[] data = FileUtil.readBytesFromUri(this, uri, 16 * 1024 * 1024);
        if (data == null) {
            displayError(getString(R.string.memory_card_editor_import_card_read_failed, uri.toString()));
            return;
        }

        String importFileName = FileUtil.getDocumentNameFromUri(this, uri);
        if (importFileName == null) {
            importFileName = uri.getPath();
            if (importFileName == null || importFileName.isEmpty())
                importFileName = uri.toString();
        }

        final String captureImportFileName = importFileName;
        final AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setMessage(getString(R.string.memory_card_editor_import_card_confirm_message,
            importFileName, card.getTitle()));
        builder.setPositiveButton(R.string.main_activity_yes, (dialog, which) -> {
            dialog.dismiss();

            if (!card.importCard(captureImportFileName, data)) {
                displayError(getString(R.string.memory_card_editor_import_failed));
                return;
            }

            refreshView(card);
        });
        builder.setNegativeButton(R.string.main_activity_no, (dialog, which) -> dialog.dismiss());
        builder.create().show();
    }

    private void formatCard() {
        final MemoryCardImage card = getCurrentCard();
        if (card == null) {
            displayMessage(getString(R.string.memory_card_editor_no_card_selected));
            return;
        }

        final AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setMessage(getString(R.string.memory_card_editor_format_card_confirm_message, card.getTitle()));
        builder.setPositiveButton(R.string.main_activity_yes, (dialog, which) -> {
            dialog.dismiss();

            if (!card.format()) {
                displayError(getString(R.string.memory_card_editor_format_card_failed, card.getUri().toString()));
                return;
            }

            displayMessage(getString(R.string.memory_card_editor_format_card_success, card.getUri().toString()));
            refreshView(card);
        });
        builder.setNegativeButton(R.string.main_activity_no, (dialog, which) -> dialog.dismiss());
        builder.create().show();
    }

    private void deleteCard() {
        final MemoryCardImage card = getCurrentCard();
        if (card == null) {
            displayMessage(getString(R.string.memory_card_editor_no_card_selected));
            return;
        }

        final AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setMessage(getString(R.string.memory_card_editor_delete_card_confirm_message, card.getTitle()));
        builder.setPositiveButton(R.string.main_activity_yes, (dialog, which) -> {
            dialog.dismiss();

            if (!card.delete()) {
                displayError(getString(R.string.memory_card_editor_delete_card_failed, card.getUri().toString()));
                return;
            }

            displayMessage(getString(R.string.memory_card_editor_delete_card_success, card.getUri().toString()));
            cards.remove(card);
            refreshView(-1);
        });
        builder.setNegativeButton(R.string.main_activity_no, (dialog, which) -> dialog.dismiss());
        builder.create().show();
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        switch (requestCode) {
            case REQUEST_IMPORT_CARD: {
                if (resultCode != RESULT_OK)
                    return;

                importCard(data.getData());
            }
            break;
        }
    }

    private static class SaveViewHolder extends RecyclerView.ViewHolder implements View.OnClickListener {
        private MemoryCardEditorActivity mParent;
        private View mItemView;
        private MemoryCardImage mCard;
        private MemoryCardFileInfo mFile;

        public SaveViewHolder(MemoryCardEditorActivity parent, @NonNull View itemView) {
            super(itemView);
            mParent = parent;
            mItemView = itemView;
            mItemView.setOnClickListener(this);
        }

        public void bindToEntry(MemoryCardImage card, MemoryCardFileInfo file) {
            mCard = card;
            mFile = file;

            ((TextView) mItemView.findViewById(R.id.title)).setText(mFile.getTitle());
            ((TextView) mItemView.findViewById(R.id.filename)).setText(mFile.getFilename());

            final String blocksText = String.format("%d Blocks", mFile.getNumBlocks());
            final String sizeText = String.format("%.1f KB", (float)mFile.getSize() / 1024.0f);
            ((TextView) mItemView.findViewById(R.id.block_size)).setText(blocksText);
            ((TextView) mItemView.findViewById(R.id.file_size)).setText(sizeText);

            if (mFile.getNumIconFrames() > 0) {
                final Bitmap bitmap = mFile.getIconFrameBitmap(0);
                if (bitmap != null) {
                    ((ImageView) mItemView.findViewById(R.id.icon)).setImageBitmap(bitmap);
                }
            }
        }

        @Override
        public void onClick(View v) {
            final AlertDialog.Builder builder = new AlertDialog.Builder(mItemView.getContext());
            builder.setTitle(mFile.getFilename());
            builder.setItems(R.array.memory_card_editor_save_menu, ((dialog, which) -> {
                switch (which) {
                    // Copy Save
                    case 0: {
                        dialog.dismiss();
                        mParent.copySave(mCard, mFile);
                    }
                    break;

                    // Delete Save
                    case 1: {
                        dialog.dismiss();
                        mParent.deleteSave(mCard, mFile);
                    }
                    break;
                }
            }));
            builder.create().show();
        }
    }

    private static class SaveViewAdapter extends RecyclerView.Adapter<SaveViewHolder> {
        private MemoryCardEditorActivity parent;
        private MemoryCardImage card;
        private MemoryCardFileInfo[] files;

        public SaveViewAdapter(MemoryCardEditorActivity parent, MemoryCardImage card) {
            this.parent = parent;
            this.card = card;
            this.files = card.getFiles();
        }

        @NonNull
        @Override
        public SaveViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            final View rootView = LayoutInflater.from(parent.getContext()).inflate(R.layout.layout_memory_card_save, parent, false);
            return new SaveViewHolder(this.parent, rootView);
        }

        @Override
        public void onBindViewHolder(@NonNull SaveViewHolder holder, int position) {
            holder.bindToEntry(card, files[position]);
        }

        @Override
        public int getItemCount() {
            return (files != null) ? files.length : 0;
        }

        @Override
        public int getItemViewType(int position) {
            return R.layout.layout_memory_card_save;
        }
    }

    public static class MemoryCardFileFragment extends Fragment {
        private MemoryCardEditorActivity parent;
        private MemoryCardImage card;
        private SaveViewAdapter adapter;
        private RecyclerView recyclerView;

        public MemoryCardFileFragment(MemoryCardEditorActivity parent, MemoryCardImage card) {
            this.parent = parent;
            this.card = card;
        }

        @Nullable
        @Override
        public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container, @Nullable Bundle savedInstanceState) {
            return inflater.inflate(R.layout.fragment_memory_card_file, container, false);
        }

        @Override
        public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
            adapter = new SaveViewAdapter(parent, card);
            recyclerView = view.findViewById(R.id.recyclerView);
            recyclerView.setAdapter(adapter);
            recyclerView.setLayoutManager(new LinearLayoutManager(recyclerView.getContext()));
            recyclerView.addItemDecoration(new DividerItemDecoration(recyclerView.getContext(),
                    DividerItemDecoration.VERTICAL));
        }
    }

    public static class CollectionAdapter extends FragmentStateAdapter {
        private MemoryCardEditorActivity parent;
        private final TabLayoutMediator.TabConfigurationStrategy tabConfigurationStrategy = (tab, position) -> {
            tab.setText(parent.cards.get(position).getTitle());
        };

        public CollectionAdapter(MemoryCardEditorActivity parent) {
            super(parent);
            this.parent = parent;
        }

        public TabLayoutMediator.TabConfigurationStrategy getTabConfigurationStrategy() {
            return tabConfigurationStrategy;
        }

        @NonNull
        @Override
        public Fragment createFragment(int position) {
            return new MemoryCardFileFragment(parent, parent.cards.get(position));
        }

        @Override
        public int getItemCount() {
            return parent.cards.size();
        }
    }
}