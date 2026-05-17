# Water Heating & Monitoring System — Flowchart

## 1. Main System Flow

```mermaid
flowchart TD
    START([🔌 تشغيل الجهاز]) --> INIT[تهيئة الأطراف والشاشة والحساسات\nكل الريلايات = OFF]
    INIT --> SHOW_WATER[عرض شاشة إدخال نسبة الماء]

    SHOW_WATER --> READ_KEY{قراءة\nالكيبورد}

    READ_KEY -- A --> SCREEN_WATER[شاشة إدخال نسبة الماء]
    READ_KEY -- D --> SCREEN_TEMP[شاشة إدخال درجة الحرارة]
    READ_KEY -- C --> SCREEN_MAIN[الشاشة الرئيسية]
    READ_KEY -- لا يوجد --> READ_SENSORS

    SCREEN_WATER --> INPUT_WATER[/إدخال رقم 0-100/]
    INPUT_WATER --> VALIDATE{الرقم\n> 100؟}
    VALIDATE -- نعم --> ERR1[عرض خطأ\nأدخل مرة أخرى]
    ERR1 --> INPUT_WATER
    VALIDATE -- لا --> SAVE_WATER[حفظ نسبة الماء المطلوبة]
    SAVE_WATER --> SCREEN_MAIN

    SCREEN_TEMP --> INPUT_TEMP[/إدخال درجة الحرارة/]
    INPUT_TEMP --> SAVE_TEMP[حفظ درجة الحرارة المطلوبة]
    SAVE_TEMP --> SCREEN_MAIN

    SCREEN_MAIN --> READ_SENSORS[قراءة الحساسات\nالترا سونك + DS18B20 + العكورة]
    READ_SENSORS --> UPDATE_LCD[تحديث الشاشة الرئيسية\nنسبة الماء / الحرارة / العكورة]
    UPDATE_LCD --> CHECK_TURBID{هل الماء\nملوث؟}

    CHECK_TURBID -- نعم --> CLEAN_MODE
    CHECK_TURBID -- لا --> NORMAL_MODE

    CLEAN_MODE --> LOOP_BACK
    NORMAL_MODE --> LOOP_BACK
    LOOP_BACK([🔁 العودة للبداية]) --> READ_KEY
```

---

## 2. Water Level Control (Normal Mode)

```mermaid
flowchart TD
    A([دخول التحكم الطبيعي]) --> B{هل تم تحديد\nنسبة الماء؟}
    B -- لا --> B2[إيقاف كلا المضختين] --> Z
    B -- نعم --> C{مقارنة المستوى\nالحالي بالمطلوب}

    C -- المستوى أقل من\nالهدف - 2% --> D[تشغيل مضخة الإدخال ⬆️\nإيقاف مضخة الإخراج]
    C -- المستوى أعلى من\nالهدف + 2% --> E[تشغيل مضخة الإخراج ⬇️\nإيقاف مضخة الإدخال]
    C -- المستوى ضمن\nنطاق ±2% --> F{هل وصل\nللهدف لأول مرة؟}

    F -- نعم --> G[إيقاف كلا المضختين\nتشغيل الجرس 1.5 ثانية 🔔\nعرض id على الشاشة]
    F -- لا --> H[إيقاف كلا المضختين]

    D --> Z([العودة])
    E --> Z
    G --> Z
    H --> Z
    B2 --> Z
```

---

## 3. Heater Control

```mermaid
flowchart TD
    A([تحكم الهيتر]) --> B{هل تم تحديد\nدرجة الحرارة؟}
    B -- لا --> OFF[إيقاف الهيتر ❌] --> Z
    B -- نعم --> C{نسبة الماء\n≥ 50%؟}

    C -- لا --> OFF2[إيقاف الهيتر ❌\nلحماية عنصر التسخين] --> Z
    C -- نعم --> D{الحرارة الحالية\n< المطلوبة؟}

    D -- نعم --> ON[تشغيل الهيتر 🔥] --> Z
    D -- لا --> OFF3[إيقاف الهيتر ✅\nتم الوصول للحرارة] --> Z

    Z([العودة])
```

---

## 4. Turbidity Cleaning Mode

```mermaid
flowchart TD
    A([كشف العكورة 🚨]) --> B{أول مرة\nيُكشف التلوث؟}
    B -- نعم --> C[تشغيل الجرس 3 ثوانٍ 🔔\nتفعيل وضع التنظيف]
    B -- لا --> D

    C --> D[إيقاف الهيتر ❌]
    D --> E{نسبة الماء\nالحالية}

    E -- ≥ 50% --> F[تشغيل مضخة الإدخال ⬆️\nتشغيل مضخة الإخراج ⬇️\nتدوير الماء للتنظيف]

    E -- بين 48% و50% --> G[إيقاف كلا المضختين\nانتظار الاستقرار]

    E -- < 48% --> H[تشغيل مضخة الإدخال فقط ⬆️\nالرجوع إلى 50%]

    F --> CHECK{هل اختفت\nالعكورة؟}
    G --> CHECK
    H --> CHECK

    CHECK -- نعم --> EXIT[الخروج من وضع التنظيف\nاستئناف التحكم الطبيعي ✅]
    CHECK -- لا --> E

    EXIT --> Z([العودة])
```

---

## 5. Keypad Navigation Map

```mermaid
flowchart LR
    MAIN[🖥️ الشاشة الرئيسية\nعرض: نسبة الماء\nدرجة الحرارة\nحالة العكورة]

    WATER[💧 إدخال نسبة الماء\n0 - 100%]
    TEMP[🌡️ إدخال درجة الحرارة\nبالدرجة المئوية]

    MAIN -- A --> WATER
    MAIN -- D --> TEMP
    WATER -- C --> MAIN
    TEMP -- C --> MAIN
    WATER -- # تأكيد --> MAIN
    TEMP -- # تأكيد --> MAIN
    WATER -- * مسح --> WATER
    TEMP -- * مسح --> TEMP
```

---

## 6. Buzzer Trigger Summary

```mermaid
flowchart LR
    B1[✅ وصول الماء للمستوى المطلوب] -- 1.5 ثانية --> BUZ[🔔 الجرس]
    B2[⚠️ كشف تلوث العكورة] -- 3 ثوانٍ --> BUZ
```
