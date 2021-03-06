/*
 * This file is part of the xTuple ERP: PostBooks Edition, a free and
 * open source Enterprise Resource Planning software suite,
 * Copyright (c) 1999-2019 by OpenMFG LLC, d/b/a xTuple.
 * It is licensed to you under the Common Public Attribution License
 * version 1.0, the full text of which (including xTuple-specific Exhibits)
 * is available at www.xtuple.com/CPAL.  By using this software, you agree
 * to be bound by its terms.
 */

#include "dspCountTagsByClassCode.h"

dspCountTagsByClassCode::dspCountTagsByClassCode(QWidget* parent, const char*, Qt::WindowFlags fl)
  : dspCountTagsBase(parent, "dspCountTagsByClassCode", fl)
{
  setWindowTitle(tr("Count Tags by Class Code"));
  setReportName("CountTagsByClassCode");
  _parameter->setType(ParameterGroup::ClassCode);
  _itemGroup->hide();
}
