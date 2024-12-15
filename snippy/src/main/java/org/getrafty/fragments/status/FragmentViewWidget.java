package org.getrafty.fragments.status;

import com.intellij.openapi.project.Project;
import com.intellij.openapi.wm.StatusBarWidget;
import org.getrafty.fragments.services.FragmentsManager;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.awt.*;

public class FragmentViewWidget implements StatusBarWidget, StatusBarWidget.TextPresentation {
    public static final String FRAGMENT_VIEW_WIDGET = "FragmentViewWidget";

    private final FragmentsManager fragmentsManager;

    public FragmentViewWidget(@NotNull Project project) {
        this.fragmentsManager = project.getService(FragmentsManager.class);
    }

    @NotNull
    @Override
    public String ID() {
        return FRAGMENT_VIEW_WIDGET;
    }

    @Nullable
    @Override
    public WidgetPresentation getPresentation() {
        return this;
    }

    @NotNull
    @Override
    public String getText() {
        return "Fragment view: " + (fragmentsManager.isMaintainerMode() ? "Maintainer" : "User");
    }


    @Nullable
    @Override
    public String getTooltipText() {
        return getText();
    }

    @Override
    public float getAlignment() {
        return Component.CENTER_ALIGNMENT;
    }
}