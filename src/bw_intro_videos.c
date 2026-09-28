#include "global.h"
#include "main.h"
#include "task.h"
#include "sound.h"
#include "constants/songs.h"
#include "task.h"
#include "intro.h"
#include "m4a.h"
#include "sound.h"
#include "constants/songs.h"

#include "logo_video.h"
#include "coronation_video.h"
#include "bw_intro_videos.h"
#include "title_screen.h"

#define SECONDS_TO_FRAMES(s) ((s) * 60)

enum { SEQ_LOGO, SEQ_CORONATION, SEQ_DONE };
static u8  sSeqVideo = SEQ_LOGO;
static u8  sSeqState = 0;
static u16 sCoronationBgmTimer = 0;
static bool8 sTeaserBgmStarted = FALSE;


#define CORONATION_TEASER_CUE_SECONDS 45

void CB2_PlayIntroVideos(void)
{

    switch (sSeqVideo)
    {
        
    case SEQ_LOGO:
        switch (sSeqState)
        {
        case 0:
            ResetTasks();
            PlayBGM(MUS_SEQ_BGM_GF_LOGO);           // logo theme, fully independent
            if (LogoVideoPlayer_Start() != TASK_NONE)
                sSeqState = 1;
            break;
        case 1:
            RunTasks();
            if (LogoVideoPlayer_IsDone())
            {
                sSeqState = 0;
                sSeqVideo = SEQ_CORONATION;
            }
            break;
        }
        break;

    case SEQ_CORONATION:
        switch (sSeqState)
         {
        case 0:
            ResetTasks();
            PlayBGM(MUS_SEQ_BGM_TITLE);           
            if (CoronationVideoPlayer_Start() != TASK_NONE)
                sSeqState = 1;
            break;
        case 1:
            RunTasks();
            if (CoronationVideoPlayer_IsDone())
            {
                sSeqState = 0;
                sSeqVideo = SEQ_DONE;
            }
            break;
        }
        break;

    case SEQ_DONE:
        SetMainCallback2(CB2_InitTitleScreen);
        break;
    }
}