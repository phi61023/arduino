//======================= DBG & TIME ======================= START
#include <Wire.h>
#include "RTClib.h"
RTC_DS3231 rtc;

char buf[256];
bool need_print_time = false;
#define DBG(x, ...) ({ \
  if (need_print_time) \
    print_time(); \
  snprintf(buf, sizeof(buf),x,##__VA_ARGS__); \
  if (buf[strlen(buf)-1] == '\n') \
    need_print_time = true; \
  else \
    need_print_time = false; \
  Serial.print(buf); \
})

void set_time()
{
  /* It will start at "2000-01-01 00:00:00" if no battery. */
  int time_offset = 11; // add 6 seconds for compile and upload time.
  DBG("Set RTC using PC Time.\n");
  DBG("Date = %s\n", __DATE__);
  DBG("Time = %s\n", __TIME__);
  DBG("TIme Offset = %d\n", time_offset);
  DBG("Write to RTC ... ");
  rtc.adjust(DateTime(F(__DATE__), F(__TIME__)) + time_offset);
  DBG("ok\n");
}

void print_time()
{
  char time_str[32] = "xxxx-xx-xx xx:xx:xx";
  DateTime now = rtc.now();
  if (now.month() != 165 && now.day() != 165)
  {
    snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d ",
    now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  }
  Serial.print(time_str);
}
//======================= DBG & TIME ======================= END

#include <SoftwareSerial.h>

#define PMS_RX_PIN                  2
#define PMS_TX_PIN                  3
#define LCM_RX_PIN                  4
#define LCM_TX_PIN                  5
#define USER_LED_PIN                6
#define SRP_RX_PIN                  8
#define SRP_TX_PIN                  9
#define RELAY_PIN                   11
#define BUZZER_PIN                  12

#define SENSOR_DATA_GET_COUNT       100 //100 is about 5 seconds get each data.
#define HISTORY_MAX_COUNT           100 //100 is about 10 minutes.
#define ALERT_MAX_COUNT             5000 //100 is about 10 minutes.
#define BUZZER_FREQ_L               100
#define BUZZER_FREQ_H               2000 // original is 4000, but 2000 is louder.

#define ALERT_UPPER_BOUND           60
#define PRINT_TH                    0

/* Global variables */
unsigned long drop = 0;
unsigned long err = 0;
unsigned long alert_period = 0;
unsigned long safe_period = 0;
unsigned long alert_period_total = 0;
unsigned long safe_period_total = 0;

int alert_threshold = 10;
int history_value[HISTORY_MAX_COUNT];
int history_count = 0;
int history_round = 0;
int skip_first_check = 0;

int g_total = 0;
int g_count = 0;

int safe_count = 0;
int alert_count = ALERT_MAX_COUNT;
int alert_status = 1;
int combo_alert_flag = 0;
int combo_alert_count = 0;
int alert_highest_value = 0;
int alert_average_value = 0;
int alert_last_value = 0;
int boundary_error = 0;

char lcm_buf[32];

SoftwareSerial lcm_Serial(LCM_RX_PIN, LCM_TX_PIN);
SoftwareSerial srp_Serial(SRP_RX_PIN, SRP_TX_PIN);
SoftwareSerial pms_Serial(PMS_RX_PIN, PMS_TX_PIN);

void lcm_display(int line, char *string)
{
  int i;
  char buf[32];
  int len = strlen(string);

  if (len > 16)
  {
    len = 16;
  }
  buf[0] = 0x4d;
  buf[1] = 0x0c;
  if (line == 1)
  {
    buf[2] = 0x01;
  }
  else
  {
    buf[2] = 0x00;
  }
  buf[3] = 16;
  strncpy(buf+4, string, len);
  for (i = 4+len; i < 20; i++)
  {
    buf[i] = ' ';
  }
  lcm_Serial.write(buf, 20);
}

void lcm_light_on()
{
  char cmd[3] = {0x4d,0x5e,0x01};
  lcm_Serial.write(cmd, 3);
  DBG("lcm_light_on\n");
}

void lcm_light_off()
{
  char cmd[3] = {0x4d,0x5e,0x00};
  lcm_Serial.write(cmd, 3);
  DBG("lcm_light_off\n");
}

int led(int pin)
{
  digitalWrite(pin, HIGH);
  delay(5);
  digitalWrite(pin, LOW);
}

int beep(int n, int freq, int beep_length=100)
{
  int i;
  for (i = 0; i< n; i++)
  {
    if (i > 0)
    {
      delay(100);
    }
    tone(BUZZER_PIN, freq);
    delay(beep_length);
    noTone(BUZZER_PIN);
  }
}

int divide(int c1, int c2)
{
  int ret = c1 / (c2 == 0 ? 1 : c2);
  int mod = c1 % c2;

  g_total = c1;
  g_count = c2;

  if (mod > (c2 / 2))
  {
    ret++;
  }
  //if (alert_count > PRINT_TH)
  //{
    //DBG("avg(%d/%d)=%d", c1, c2, ret);
  //}
  return ret;
}

int get_history_avg()
{
  int i;
  int total = 0;
  if (history_round)
  {
    for (i = 0; i < HISTORY_MAX_COUNT; i++)
    {
      total += history_value[i];
    }
    return divide(total, HISTORY_MAX_COUNT);
  }
  else
  {
    for (i = 0; i < history_count; i++)
    {
      total += history_value[i];
    }
    return divide(total, history_count);
  }
}

int check_value(int cur_value)
{
  int his_avg = get_history_avg();
  int add_history_count = 0;
  int add_safe_count = 0;

  //if (alert_count > PRINT_TH)
  //{
    //DBG(", cur=%d, err=%lu, drop=%lu", cur_value, err, drop);
  //}

  if (his_avg == 0)
  {
    //DBG(", h[%d]=%d\n", history_count, cur_value);
    history_value[history_count] = cur_value;
    history_count++;
    return 0;
  }

  if (history_count >= HISTORY_MAX_COUNT)
  {
    history_count = 0;
    history_round = 1;
  }

  if (skip_first_check < 10)
  {
    skip_first_check++;
    history_value[history_count] = cur_value;
    DBG("h[%d]=%d, total=%d, skip_first_check=%d\n", history_count, history_value[history_count], g_total, skip_first_check);
    snprintf(lcm_buf, sizeof(lcm_buf), "%d a=%d %d,%d", cur_value, his_avg, safe_count, alert_count);
    lcm_display(0, lcm_buf);
    snprintf(lcm_buf, sizeof(lcm_buf), "h[%d]=%d %d", history_count, history_value[history_count], g_total);
    lcm_display(1, lcm_buf);
    history_count++;
    //beep(1, BUZZER_FREQ_H, 200);
    return 0;
  }

  if (alert_status > 0)
  {
    alert_period++;
    alert_period_total++;
  }
  else
  {
    safe_period++;
    safe_period_total++;
  }
  alert_highest_value = cur_value > alert_highest_value ? cur_value : alert_highest_value;
  if (cur_value >= (his_avg + alert_threshold) || cur_value > ALERT_UPPER_BOUND)
  {
    safe_count = 0;
    if (alert_status == 0 && alert_count >= 0)
    {
      alert_status = 1;
      alert_highest_value = cur_value;
      alert_average_value = his_avg;
      //DBG("\n");
      DBG("---- Alert, ath=%d, cur=%d, avg=%d, safe_period=%lu\n", alert_threshold, cur_value, his_avg, safe_period);
      safe_period = 0;
      digitalWrite(RELAY_PIN, LOW);
      digitalWrite(USER_LED_PIN, HIGH);
    }
    alert_count++;
    if (combo_alert_flag > 0)
    {
      combo_alert_count++;
    }
    else
    {
      combo_alert_flag++;
    }
    if (alert_last_value == 0)
    {
      alert_last_value = cur_value;
    }
    else
    if (cur_value - alert_last_value > 0)
    {
      beep(1, BUZZER_FREQ_H, 200);
      alert_last_value = cur_value;
    }
  }
  else
  if (cur_value <= his_avg + 1)
  {
    if (alert_status > 0)
    {
      safe_count++;
      add_safe_count = 1;
      if (safe_count > (alert_count > 1 ? 60 : 20))
      {
        //DBG("\n");
        DBG("----- Safe, ath=%d, cur=%d, avg=%d, ahv=%d, alert_period=%lu\n", alert_threshold, cur_value, his_avg, alert_highest_value, alert_period);
        alert_period = 0;
        if (alert_highest_value - alert_average_value - alert_threshold <= 3 &&
            alert_highest_value - alert_average_value - alert_threshold >= 0)
        {
          DBG("# alert_threshold: %d -> %d\n", alert_threshold, alert_threshold + 1);
          alert_threshold++;
        }
        alert_status = 0;
        alert_count = 0;
        safe_count = 0;
        alert_highest_value = 0;
        alert_average_value = 0;
        alert_last_value = 0;
        beep(2, BUZZER_FREQ_H);
        digitalWrite(USER_LED_PIN, LOW);
        digitalWrite(RELAY_PIN, HIGH);
      }
    }
    combo_alert_flag = 0;
    combo_alert_count = 0;
  }
  else
  {
    combo_alert_flag = 0;
    combo_alert_count = 0;
  }

  //if (alert_count > PRINT_TH)
  //{
    //DBG(", safe=%d, alert=%d, caf=%d, cac=%d, ahv=%d, ath=%d", safe_count, alert_count, combo_alert_flag, combo_alert_count, alert_highest_value, alert_threshold);
  //}
  snprintf(lcm_buf, sizeof(lcm_buf), "%d a=%d %d,%d,%d", cur_value, his_avg, safe_count, alert_count, combo_alert_count);
  lcm_display(0, lcm_buf);

  if (alert_status == 0 || alert_count >= ALERT_MAX_COUNT || add_safe_count > 0)
  {
    history_value[history_count] = cur_value;
    //if (alert_count > PRINT_TH)
    //{
      //DBG(", h[%d]=%d, total=%d\n", history_count, history_value[history_count], g_total);
    //}
    snprintf(lcm_buf, sizeof(lcm_buf), "h[%d]=%d %d", history_count, history_value[history_count], g_total);
    lcm_display(1, lcm_buf);
    add_history_count = 1;
    history_count++;
  }

  //if (add_history_count == 0)
  //{
    //if (alert_count > PRINT_TH)
    //{
      //DBG("\n");
    //}
  //}
}

void setup()
{
  rtc.begin();
  //set_time();
  lcm_Serial.begin(1200);
  srp_Serial.begin(2400);
  pms_Serial.begin(9600);
  Serial.begin(9600);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(USER_LED_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(USER_LED_PIN, HIGH);
}

/* loop() local variables. */
bool first_beep = false;
unsigned char data[32];
unsigned int sum = 0;
int val_1 = 0;
int val_2 = 0;
int p = 0;

void loop()
{
  digitalWrite(LED_BUILTIN, LOW);
  while (pms_Serial.available() > 0)
  {
    digitalWrite(LED_BUILTIN, HIGH);
    data[p] = pms_Serial.read();
    if (p == 0 && data[p] != 0x42)
    {
      DBG("drop data[%02d] = 0x%02x\n", p, data[p]);
      drop++;
      sum = 0;
      break;
    }
    if (p == 1 && data[p] != 0x4d)
    {
      DBG("drop data[%02d] = 0x%02x\n", p, data[p]);
      drop++;
      sum = 0;
      p = 0;
      break;
    }
    if (p < 30)
    {
        sum += data[p];
    }
    if (p == 31)
    {
      if (sum == (data[30] << 8) + data[31]) // cksum ok.
      {
        val_1 = (data[8] << 8) + data[9]; // PM10 (CF=1)
        val_2 = (data[16] << 8) + data[17]; // >0.3um count
        if (val_1 < 256 && val_2 > 0)
        {
          boundary_error = 0;
          /*DBG("p=[%d, %d, %d], c1=[%d, %d, %d], c2=[%d, %d, %d], [0x%02x], [0x%02x], drop=%lu, err=%lu\n",
          (data[4] << 8) + data[5],
          (data[6] << 8) + data[7],
          (data[8] << 8) + data[9],
          (data[16] << 8) + data[17],
          (data[18] << 8) + data[19],
          (data[20] << 8) + data[21],
          (data[22] << 8) + data[23],
          (data[24] << 8) + data[25],
          (data[26] << 8) + data[27],
          data[28],
          data[29],
          drop,
          err);*/
          val_1++; // val_1 is 1 at least.
          if (first_beep == false)
          {
            first_beep = true;
            DBG("First value = %d\n", val_1);
            beep(1, BUZZER_FREQ_H, 200);
            lcm_display(0, "Start...");
            snprintf(lcm_buf, sizeof(lcm_buf), "value = %d", val_1);
            lcm_display(1, lcm_buf);
          }
          check_value(val_1);
        }
        else
        {
          boundary_error++;
          if (boundary_error > 100)
          {
            beep(1, BUZZER_FREQ_H, 100);
            DBG("ERROR: boundary error, val_1=%d, val_2=%d\n", val_1, val_2);
          }
        }
      }
      else // cksum error.
      {
        DBG("ERROR: cksum error. actual_sum=0x%04x, expect_sum=0x%04x\n", sum, (data[30] << 8) + data[31]);
        err++;
      }
      sum = 0;
      p = 0;
      break;
    }
    p++;
  }
}

