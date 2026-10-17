// From Dusklight (CC0), src/dusk/ui/group_button.cpp, by way of FFCCEOT-Recomp. Adapted for Demon's Souls (unchanged here).
#include "group_button.hpp"

namespace ui {

GroupButton::GroupButton(Rml::Element* parent, Props props)
    : SelectButton{parent, {.key = std::move(props.text)}},
      mIsSelected{std::move(props.isSelected)}, mIsDisabled{std::move(props.isDisabled)} {
    mRoot->SetClass("group-button", true);
}

void GroupButton::update() {
    set_selected(selected());
    set_disabled(disabled());
    SelectButton::update();
}

bool GroupButton::selected() const {
    return mIsSelected ? mIsSelected() : SelectButton::selected();
}

bool GroupButton::disabled() const {
    return mIsDisabled ? mIsDisabled() : SelectButton::disabled();
}

}  // namespace ui
