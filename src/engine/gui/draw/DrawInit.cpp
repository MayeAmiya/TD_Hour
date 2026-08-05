#include "DrawFunc.h"
#include "../../../core/constants/Strings.h"

namespace gui::draw {

void initPushButtonDrawFuncs();
void initRadioButtonDrawFuncs();
void initCheckBoxDrawFuncs();
void initTextEntryDrawFuncs();
void initProgressBarDrawFuncs();
void initSliderDrawFuncs();
void initListBoxDrawFuncs();
void initComboBoxDrawFuncs();
void initDefaultDrawFuncs();

void initDrawFuncs() {
    initPushButtonDrawFuncs();
    initRadioButtonDrawFuncs();
    initCheckBoxDrawFuncs();
    initTextEntryDrawFuncs();
    initProgressBarDrawFuncs();
    initSliderDrawFuncs();
    initListBoxDrawFuncs();
    initComboBoxDrawFuncs();
    initDefaultDrawFuncs();
}

} // namespace gui::draw
