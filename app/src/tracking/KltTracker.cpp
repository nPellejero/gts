/*
 * Copyright (C) 2007-2013 Dyson Technology Ltd, all rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "KltTracker.h"
#include "CameraCalibration.h"
#include "RobotMetrics.h"
#include "CrossCorrelation.h"

#include "OpenCvUtility.h"

#include "Angles.h"

#include "MathsConstants.h"

#include "Logging.h"

#include <opencv/cxcore.h>
#include <opencv/highgui.h>
#include <unistd.h>

#include "time.h"
using namespace std;
/**
 This tracker must be constructed by passing in camera calibration information
 and robot metrics.

 There are a number of other parameters which must also be set before tracking
 is started:
 SetPosition() - initial position,
 SetCurrentImage() - image to track in.
 **/

KltTracker::KltTracker( const CameraCalibration* cal,
                        const RobotMetrics* metrics,
                        const IplImage* currentImage,
                        int thresh ) :
    RobotTracker    (),
    m_pos           ( cvPoint2D32f( 0.f, 0.f ) ),
    m_angle         ( 0.f ),
    m_thresh1       ( thresh ),
    m_error         ( 0.f ),
    m_nccThresh     ( 0.4f ),
    m_currImg       ( 0 ),
    m_prevImg       ( 0 ),
    m_currPyr       ( 0 ),
    m_prevPyr       ( 0 ),
    m_weightImg     ( 0 ),
    m_targetImg     ( 0 ),
    m_appearanceImg ( 0 ),
    m_avgFloat      ( 0 ),
    m_avg           ( 0 ),
    m_diff          ( 0 ),
    m_filtered      ( 0 ),
    m_history       (),
    m_cal           ( cal ),
    m_metrics       ( metrics )
{
    //assert(cal && metrics );
    CreateWeightImage();

    LOG_INFO(QObject::tr("Bi-level threshold: %1.").arg(m_thresh1));

    SetCurrentImage( currentImage );
}

KltTracker::~KltTracker()
{
    ReleasePyramids();
    ReleaseWeightImage();
    cvReleaseImage( &m_targetImg );
    cvReleaseImage( &m_appearanceImg );

    cvReleaseImage( &m_avgFloat );
    cvReleaseImage( &m_avg );
    cvReleaseImage( &m_diff );
}

/**
 Returns the position of the left edge of the brushbar
 (given that robot has the specified position and heading).
 **/
CvPoint2D32f KltTracker::GetBrushBarLeft( CvPoint2D32f position, float heading ) const
{
    heading += MathsConstants::F_PI / 2.f;

    float w = GetMetrics()->GetBrushBarWidthPx(); // brush bar width (pixels)

    float px = -w / 2.f; // half bbar width
    float py = m_metrics->GetBrushBarOffsetPx(); // offset of brush bar in direction of travel (from centre of robot)

    float cosa = cos( -heading );
    float sina = sin( -heading );

    float x = px * cosa - py * sina;
    float y = px * sina + py * cosa;

    return cvPoint2D32f( position.x + x, position.y + y );
}

/**
 Returns the position of the right edge of the brushbar
 (given that robot has the specified position and heading).
 **/
CvPoint2D32f KltTracker::GetBrushBarRight( CvPoint2D32f position, float heading ) const
{
    heading += 3.14159265359f / 2.f;

    float w = GetMetrics()->GetBrushBarWidthPx(); // brush bar width (pixels)

    float px = w / 2.f; // half bbar width
    float py = m_metrics->GetBrushBarOffsetPx(); // offset of brush bar in direction of travel (from centre of robot)

    float cosa = cos( -heading );
    float sina = sin( -heading );

    float x = px * cosa - py * sina;
    float y = px * sina + py * cosa;

    return cvPoint2D32f( position.x + x, position.y + y );
}

/**
 Allocate KLT pyramids (only if we haven't already)
 **/
void KltTracker::AllocatePyramids()
{
    if ( !m_prevPyr )
    {
        m_prevPyr = cvCreateImage( cvSize( m_prevImg->width, m_prevImg->height ),
                                   m_prevImg->depth,
                                   m_prevImg->nChannels );
        m_currPyr = cvCreateImage( cvSize( m_currImg->width, m_currImg->height ),
                                   m_currImg->depth,
                                   m_currImg->nChannels );
    }
}

/**
 Free memory used by KLT pyramids
 **/
void KltTracker::ReleasePyramids()
{
    cvReleaseImage( &m_prevPyr );
    cvReleaseImage( &m_currPyr );
}

/**
 Swap the two klt pyramids, so that the current becomes
 the previous for use in next frame.
 **/
void KltTracker::SwapPyramids()
{
    IplImage* tmp = m_prevPyr;
    m_prevPyr = m_currPyr;
    m_currPyr = tmp;
}

/**
 Free memory from the internal weighting image.
 **/
void KltTracker::ReleaseWeightImage()
{
    cvReleaseMat( &m_weightImg );
}

/**
 Creates an image of a radial weighting function,
 given by expf( -powf(r/(m_radius-1.f),12) ).
 This allows us to do a 'soft cut-out' of the
 circular robot target.
 **/
void KltTracker::CreateWeightImage()
{
    ReleaseWeightImage();
    int radius = (int)(2.f * (m_metrics->GetRadiusPx() + .5f));
    m_weightImg = cvCreateMat( radius, radius, CV_32FC1 );

    float cx = m_weightImg->width / 2.f;
    float cy = m_weightImg->height / 2.f;
    for ( int i = 0; i < m_weightImg->width; ++i )
    {
        for ( int j = 0; j < m_weightImg->height; ++j )
        {
            float x = i - cx;
            float y = j - cy;
            float r = sqrtf( x * x + y * y );
            float w = expf( -powf( r / (m_metrics->GetRadiusPx()), 12 ) );
            cvmSet( m_weightImg, i, j, w );
        }
    }
}

void KltTracker::Activate()
{
    // run second stage of tracker in order to determine initial orientation properly
    ReleasePyramids();
    //assert( m_currImg );
    m_prevImg = m_currImg;

    if (!m_appearanceImg)
    {
        m_appearanceImg = cvCloneImage( m_currImg );
    }
      
    AllocatePyramids();

    m_status = TRACKER_ACTIVE;

    TrackStage2( m_pos, false, true );
}

/**
 Changes state from JUST_LOST to LOST (if this is the case)
 **/
void KltTracker::DoInactiveProcessing( double timeStamp )
{
    Q_UNUSED( timeStamp );
    if ( WasJustLost() )
    {
        SetLost();
    }
}

/**
 Run an interation of the tracker (tracks from previous frame to current frame).
 On each new frame, be sure to call SetCurrentImage() before calling Track().

 If this is the first time we are calling Track then we need to set init to true so
 that it generates both pyramids for the KLT. (In normal operation it can reuse the
 pyramid calculation from the previous frame.)
 **/
bool KltTracker::Track( double timestampInMillisecs, bool flipCorrect, bool init )
{
    char found = 0;
    CvPoint2D32f newPos;

    // First we do simple frame-to-frame KLT track.
    AllocatePyramids();
    SwapPyramids();

    int r = (int)(.9f * m_metrics->GetRadiusPx());

    int kltFlags = 0;
    if ( !init )
    {
        kltFlags = CV_LKFLOW_PYR_A_READY; // prev should be ready because TrackStage2 gets called in Activate().
    }

    cvCalcOpticalFlowPyrLK( m_prevImg,
                            m_currImg,
                            m_prevPyr,
                            m_currPyr,
                            &m_pos,
                            &newPos,
                            1,
                            cvSize( r, r ),
                            1,
                            &found,
                            &m_error,
                            cvTermCriteria( CV_TERMCRIT_ITER | CV_TERMCRIT_EPS, 20, 0.03 ),
                            kltFlags );
    if ( found && TrackStage2( newPos, flipCorrect, false ) )
    {
        float ncc = GetError();
	if ( ncc < m_nccThresh )
        {
	    if ( !IsLost() )
            {
		
		LOG_WARN("Lost.");

                LOG_INFO(QObject::tr("NCC value: %1 (thresh %2).").arg(ncc)
                                                                  .arg(m_nccThresh));
                SetJustLost(); // tracker has transitioned into lost state
            }
            else
            {
		LOG_WARN("Still lost.");

                SetLost(); // ttacker is still lost
            }
        }

        // Read the warp gradient magnitude at the tracked position to store in TrackEntry for later use.
        const CvMat* wgi = m_cal->GetWarpGradientImage();
        int x = static_cast<int>( m_pos.x );
        int y = static_cast<int>( m_pos.y );
        float warpGradient = TrackEntry::unknownWgm;

        if ( ( x < wgi->cols ) && ( y < wgi->rows ) )
        {

            warpGradient = CV_MAT_ELEM( *wgi, float, y, x );
        }


        // If we found a good track and the 2nd stage was a success then store the result
       // const float error = GetError();
        
        //assert( error >= -1.0 );
        //assert( error <= 1.0 );warp gradient magnitude at the tracked position to store in TrackEntry for later use.
        //const CvMat* w
	m_history.emplace_back( TrackEntry( GetPosition(), GetHeading(), GetError(), timestampInMillisecs, warpGradient ) );

        return true;
    }
    if ( !IsLost() )
    {
      LOG_WARN("Lost.");
      SetJustLost(); // tracker has transitioned into lost state
    }
    else
    {
       LOG_WARN("Still lost.");
       SetLost(); // ttacker is still lost
    }
    
    return false;
}

void KltTracker::Rewind( double timeStamp )
{
    while (!m_history.empty())
    {
        TrackEntry entry = m_history.back();

        if (entry.GetTimeStamp() > timeStamp)
        {
            m_pos = entry.GetPosition();
            m_angle = entry.GetOrientation();

            m_history.pop_back();
        }
        else
        {
            break;
        }
    }
}

/**
 Uses result of successful frame to frame KLT-track to predict appearance template and
 use it to refine the tracking. We use the tracking result of the first stage (newPos)
 to initialise 2nd stage. This helps the tracker avoid local minima.
 **/
bool KltTracker::TrackStage2( CvPoint2D32f newPos, bool flipCorrect, bool init )
{
    char found1 = 0;
    char found2 = 0;
    m_pos = newPos;

    float oldAngle = m_angle;
    float newAngle = ComputeHeading( m_pos );

    // Use heading to predict appearance
    PredictTargetAppearance( newAngle, 0 );

    float error1;
    float error2;
    CvPoint2D32f newPos2 = newPos;

    int r = (int)m_metrics->GetRadiusPx();

    int kltFlags = CV_LKFLOW_INITIAL_GUESSES;

    if ( !init )
    {
        kltFlags += CV_LKFLOW_PYR_B_READY;
    }

    cvCalcOpticalFlowPyrLK( m_appearanceImg,
                            m_currImg,
                            0,
                            m_currPyr,
                            &m_pos,
                            &newPos,
                            1,
                            cvSize( r, r ),
                            1,
                            &found1,
                            &error1,
                            cvTermCriteria( CV_TERMCRIT_ITER | CV_TERMCRIT_EPS, 20, 0.03 ),
                            kltFlags );

    // Compute tracker error using normalised-cross-correlation
    // of appearance image (at robots old position which is where the appearance was generated)
    // with current image
    float ncc1 = CrossCorrelation::Ncc2dRadial( m_appearanceImg, m_currImg, m_pos.x, m_pos.y, newPos.x, newPos.y, 2 * r, 2 * r );
    //float ncc1 = CrossCorrelation::Ncc2dRadial( m_appearanceImg, m_currImg, r, r, newPos.x, newPos.y, 2 * r, 2 * r );
    

    // Predict again with opposite orientation so we can disambiguate heading
    PredictTargetAppearance( newAngle, 180 );
    cvCalcOpticalFlowPyrLK( m_appearanceImg,
                            m_currImg,
                            0,
                            m_currPyr,
                            &m_pos,
                            &newPos2,
                            1,
                            cvSize( r, r ),
                            1,
                            &found2,
                            &error2,
                            cvTermCriteria( CV_TERMCRIT_ITER | CV_TERMCRIT_EPS, 20, 0.03 ),
                            kltFlags );

    // Compute tracker error using normalised-cross-correlation
    float ncc2 = CrossCorrelation::Ncc2dRadial( m_appearanceImg, m_currImg, m_pos.x, m_pos.y, newPos2.x, newPos2.y, 2 * r, 2 * r );

    int appearanceModelChosen = 0;
    if (found1)
    {
        // if both KLTs were successful then we chose between them based on the errors.
        if (!found2 || (ncc1 > ncc2))
        {
            SaveResult( newPos, newAngle, ncc1 );
            appearanceModelChosen = 1;
        }
    }
    if (found2)
    {
        // if both KLTs were successful then we chose between them based on the errors.
        if (!found1 || (ncc2 >= ncc1))
        {
            newAngle += MathsConstants::F_PI;
            SaveResult( newPos2, newAngle, ncc2 );
            appearanceModelChosen = 2;
        }
    }

    if (found1 || found2)
    {
        newAngle = Angles::NormAngle( newAngle ); // puts angle in range -pi to pi
        oldAngle -=  m_metrics->GetTargetRotationRad();
        oldAngle = Angles::NormAngle( oldAngle );

        if ( flipCorrect )
        {
            if ( HasFlipped( newAngle, oldAngle, 1.6 ) )
            {
                // Switch the choice of appearance model:
                if ( appearanceModelChosen  == 1 )
                {
                    SaveResult( newPos2, newAngle + MathsConstants::F_PI, ncc2 );
                }
                else
                {
                    SaveResult( newPos, newAngle - MathsConstants::F_PI, ncc1 );
                }

                m_angle = Angles::NormAngle( m_angle ); // puts angle in range -pi to pi
            }
        }

        m_angle += m_metrics->GetTargetRotationRad();

        return true;
    }

    return false;
}

/**
 Simple technique for determining robot heading using
 the distribution of light and dark pixels on the target.

 @param pos The image coordinate at which to perform the computation.
 **/
float KltTracker::ComputeHeading( CvPoint2D32f pos ) const
{
    if ( !m_currImg )
    {
        return m_angle;
    }

    float radius = m_metrics->GetRadiusPx();
    CvRect roi = cvRect( (int)(pos.x - radius),
                         (int)(pos.y - radius),
                         (int)(radius * 2),
                         (int)(radius * 2) );

    // check if out of boundaries
    if ( roi.x < 0 || roi.x > m_currImg->width - radius ||
         roi.y < 0 || roi.y > m_currImg->height - radius )
    {
        return m_angle;
    }

    // Copy region of interest around tracked position
    IplImage* tmpImg = cvCloneImage( m_currImg );
    cvSetImageROI( tmpImg, roi );

    IplImage* img = cvCreateImage( cvSize( tmpImg->roi->width,
                                           tmpImg->roi->height ), IPL_DEPTH_8U, 1 );
    cvCopyImage( tmpImg, img );
    cvReleaseImage( &tmpImg );

    float mx = 0.f; // mean x coord
    float my = 0.f; // mean y coord
    float sum = 0.f; // sum of weights
    for ( int i = 0; i < img->height; ++i )
    {
        for ( int j = 0; j < img->width; ++j )
        {
            float w = (float)cvmGet( m_weightImg, i, j );
            CvScalar v = cvGet2D( img, i, j );

            float val = (float)(v.val[0] * w); // Remove background with soft cut-out
            val = (255 * w) - val; // Invert

            if ( val > m_thresh1 ) // Threshold
            {
                w = expf( -powf( ((val - 255.f) / (50.f)), 2 ) );
                mx += i * w;
                my += j * w;
                sum += w;
            }
            else
            {
                val = 0;
            }

            cvSet2D( img, i, j, cvScalar( val ) );
        }
    }

    float invSum = 1.f / sum;
    mx *= invSum;
    my *= invSum;

    // Compute weighted covariance
    float cov[4];
    CvMat C = cvMat( 2, 2, CV_32F, cov );
    cvSetZero( &C );
    sum = 0.f;
    for ( int i = 0; i < img->height; ++i )
    {
        for ( int j = 0; j < img->width; ++j )
        {
            //float w    = cvmGet(m_weightImg,i,j);
            CvScalar v = cvGet2D( img, i, j );
            int val = (int)(v.val[0]);
            if ( val > m_thresh1 )
            {
                float vx = i - mx;
                float vy = j - my;
                float w = expf( -powf( ((val - 255.f) / (50.f)), 2 ) );
                cov[0] += w * vx * vx;
                cov[1] += w * vx * vy;
                cov[3] += w * vy * vy;
                sum += w;
            }
        }
    }
    invSum = 1.f / sum;
    cov[0] *= invSum;
    cov[1] *= invSum;
    cov[2] = cov[1];
    cov[3] *= invSum;

    // Compute principle components (eigen values/vectors using SVD)
    float w[4];
    float u[4];
    CvMat W = cvMat( 2, 2, CV_32F, w );
    CvMat U = cvMat( 2, 2, CV_32F, u );
    cvSVD( &C, &W, &U, 0, CV_SVD_MODIFY_A );

    // Store heading angle
    float angle = (float)(atan2( u[0], u[2] ) + (.25f * MathsConstants::F_PI));
    angle = (MathsConstants::F_PI / 2.0f) - angle;

    cvResetImageROI( img );
    cvReleaseImage( &img );

    return angle;
}

/**
 Loads the specified file which contains the robot-target to be used
 for tracking. The target is also scaled and rotated appropriately.
 **/
bool KltTracker::LoadTargetImage( const char* targetFilename )
{
    if ( targetFilename == 0 )
    {
        m_targetImg = 0;

        LOG_WARN("No target specified for tracker.");

        return false;
    }

    IplImage* img = cvLoadImage( targetFilename, 0 );

    if ( img )
    {
        LOG_INFO(QObject::tr("Opened target image: %1.")
                    .arg(targetFilename));

        // now transform the image
        float radius = m_metrics->GetRadiusPx();
        float size = ((2.f * radius) / sqrtf( 2.f ));
        m_targetImg = cvCreateImage( cvSize( (int)(size + .5f),
                                             (int)(size + .5f) ), IPL_DEPTH_8U, 1 );
        cvResize( img, m_targetImg );

        cvReleaseImage( &img );

        // Create a radial mask around the robot target
        float cx = m_targetImg->height / 2.f;
        float cy = m_targetImg->width / 2.f;
        for ( int i = 0; i < m_targetImg->height; ++i )
        {
            for ( int j = 0; j < m_targetImg->width; ++j )
            {
                float x = i - cx;
                float y = j - cy;
                float r = sqrtf( x * x + y * y );
                float w = expf( -powf( r / (radius + 1.f), 13.f ) );
                int val = (int)(w * cvGet2D( m_targetImg, i, j ).val[0] + (160.f * (1.f - w)));
                cvSet2D( m_targetImg, i, j, cvScalar( val ) );
            }
        }

        return true;
    }
    else
    {
        LOG_ERROR(QObject::tr("Could not open target image: %1!")
                      .arg(targetFilename));

        return false;
    }
}

/**
 Predicts the appearance of the target using computed heading, taking
 an extra parameter for specifying the position

 **/
void KltTracker::PredictTargetAppearance( float angleInRadians, float offsetAngleDegrees )
{
    if ( !m_targetImg )
        return;

    float angle = (float)((180 + MathsConstants::R2D * angleInRadians) + offsetAngleDegrees);
    int smoothing = 5;
    // Set the background 'color' that will be used for pixels in the
    // appearance model outside the radius of the target.
    cvSet( m_appearanceImg, cvScalar( m_targetBackGroundGreyLevel ) );

    float R[6];
    CvMat rot = cvMat( 2, 3, CV_32F, R );
    CvPoint2D32f centre = cvPoint2D32f( m_targetImg->width / 2.f, m_targetImg->height / 2.f );
    cv2DRotationMatrix( centre, angle, 1, &rot );
    R[2] += m_pos.x - m_targetImg->width / 2.f;
    R[5] += m_pos.y - m_targetImg->height / 2.f;
    cvWarpAffine( m_targetImg, m_appearanceImg, &rot, CV_INTER_LINEAR );

    cvSmooth( m_appearanceImg, m_appearanceImg, CV_GAUSSIAN, smoothing );
}


/**
 Predicts the appearance of the target using computed heading, using
 an extra parameter for specifying the position

 **/
void KltTracker::PredictTargetAppearance2( float angleInRadians, float offsetAngleDegrees, float x, float y )
{
    if ( !m_targetImg )
        return;

    float angle = (float)((180 + MathsConstants::R2D * angleInRadians) + offsetAngleDegrees);
    int smoothing = 5;
    // Set the background 'color' that will be used for pixels in the
    // appearance model outside the radius of the target.
    cvSet( m_appearanceImg, cvScalar( m_targetBackGroundGreyLevel ) );

    float R[6];
    CvMat rot = cvMat( 2, 3, CV_32F, R );
    CvPoint2D32f centre = cvPoint2D32f( m_targetImg->width / 2.f, m_targetImg->height / 2.f );
    cv2DRotationMatrix( centre, angle, 1, &rot );
    R[2] += x - m_targetImg->width / 2.f;
    R[5] += y - m_targetImg->height / 2.f;
    cvWarpAffine( m_targetImg, m_appearanceImg, &rot, CV_INTER_LINEAR );

    cvSmooth( m_appearanceImg, m_appearanceImg, CV_GAUSSIAN, smoothing );
}

/**
 Motion detection.

 We keep a moving average at all times in case we need to do
 loss recovery (in which case the average is used to compute
 motion in the current frame).
 **/
void KltTracker::MotionDetect()
{
    InitialiseRecoverySystem();

    cvRunningAvg( m_currImg, m_avgFloat, 0.6 );
}

/**
 Pre-allocates all images used by the relocalisation algorithm.
 **/
void KltTracker::InitialiseRecoverySystem()
{
    if ( m_avg == 0 && m_currImg )
    {
        m_avgFloat = cvCreateImage( cvSize( m_currImg->width,
                                            m_currImg->height ), IPL_DEPTH_32F, 1 );
        m_avg = cvCloneImage( m_currImg );
        cvConvertScale( m_avg, m_avgFloat, 1.0, 0.0 );
        m_diff = cvCloneImage( m_currImg );
        m_filtered = cvCreateImage( cvSize( m_currImg->width,
                                            m_currImg->height ), IPL_DEPTH_32S, 1 );
    }
}

/**
 When we lose track we first    perform motion
 detection to limit the area we have to search
 for the robot. Then we do heading computation
 and normalised cross correlation in all the
 moving areas in the image to localise the robot.
 **/
void KltTracker::LossRecovery()
{

    InitialiseRecoverySystem();
    cvAbsDiff(m_currImg, m_prevImg, m_diff);
    cvThreshold(m_diff, m_diff, 15, 255, CV_THRESH_BINARY);
    int i,j;
    cvSetZero(m_filtered);
    OpenCvUtility::MotionFilter(m_diff, m_filtered, 24, 24);
    cvConvertScale(m_filtered, m_avg, 1, 0.0);
    // Search for target in the mask region using cross-correlation
    cvThreshold(m_avg, m_avg, 200, 255, CV_THRESH_BINARY);
    // Look for target in search zone.
    IplImage* avg_copy = cvCreateImage(cvSize(m_avg->width, m_avg->height) ,m_avg->depth,m_avg->nChannels);
    int w = 100;


    CvMat* mask;
    mask = cvCreateMat( m_avg->width, m_avg->height,CV_8U);
    cvSetZero(mask);

    cvCopy(m_avg,avg_copy);


    for(i=0;i< avg_copy->width;i++)
    {
        for(j=0;j< avg_copy->height;j++)
        {
            if(sqrt(pow(fabs(i - cvFloor(m_pos.x)),2) + pow(fabs(j - cvFloor(m_pos.y)),2)) > w )
            {
                //LOG_INFO(QObject::tr("located at %1 %2. of %3 %4").arg(cvFloor(m_pos.x)).arg(cvFloor(m_pos.x)).arg(avg_copy->width).arg(avg_copy->height));
                cvSetReal2D(avg_copy, j, i, 0);
            }
        }
    }


    TargetSearch( avg_copy );

    IplImage* colImg = cvCreateImage( cvSize( m_currImg->width, m_currImg->height ), IPL_DEPTH_8U, 3 );
    cvCvtColor( m_currImg, colImg, CV_GRAY2RGB );

    cvSetImageCOI( colImg, 1 );
    cvCopy( m_avg, colImg );
    cvReleaseImage(&colImg);
    
}

/**
 Perform a brute force search for the robot-localisation target
 in the image. If the optional mask is specified then only pixels
 where the mask is non-zero will be searched by the algorithm.
 **/
void KltTracker::TargetSearch( const IplImage* mask )
{
    int r = (int)m_metrics->GetRadiusPx();
    int ws = 2 * r;
    int w = mask->width;
    int h = mask->height;
    float nccThreshRecovery = 0.5;
        
    IplImage* ncc = cvCreateImage( cvSize( w, h ), IPL_DEPTH_32F, 1 );
    cvZero( ncc );
    int fStep = ncc->widthStep / sizeof(float);
    int iStep = mask->widthStep;


    float maxVal = -1.f;    
    
    CvPoint2D32f maxPos;

    //Correct m_pos when lost near the limits of the window due to offsets.
    //Otherwise it get in a state where ncc2 returns 0 and lossRecovery never
    float x_lost = std::min( std::max(float(ws/2), m_pos.x) , float(m_appearanceImg->width-ws/2));
    float y_lost = std::min( std::max(float(ws/2), m_pos.y) , float(m_appearanceImg->height-ws/2));
    
    PredictTargetAppearance2(0,0,x_lost, y_lost);
    for ( int j = r; j < h - r; j = j+2 )
    {
        for ( int i = r; i < w - r; i = i + 2 )
        {
            char* pMask = mask->imageData;
            float* pNcc = reinterpret_cast< float* > ( ncc->imageData );
	    
            pMask += j * iStep + i;
            pNcc += j * fStep + i;
	    if ( *pMask )
	    { 
	      //Compute the heading of the robot for a given candidate position 
	      // and creates a patch taking into account the orientation
	      float newAngle = ComputeHeading(cvPoint2D32f( (float)i, (float)j ));
	      PredictTargetAppearance2(newAngle,0,x_lost, y_lost);
	      //Compare candidate with target appearance
	      float val = CrossCorrelation::Ncc2dRadial( m_appearanceImg, m_currImg, x_lost, y_lost, i, j, ws, ws );
	      *pNcc = val;
	      if ( val > maxVal )
	      {
		maxVal = val;
		maxPos.x = i;
		maxPos.y = j;
		m_angle = newAngle;
	       }
	    }
        }
    }
    cvReleaseImage( &ncc );
    if ( maxVal > nccThreshRecovery )
    {
        LOG_INFO(QObject::tr("Relocalised at %1 %2 (score: %3).").arg(maxPos.x)
                                                                 .arg(maxPos.y)
                                                                 .arg(maxVal));
        SetPosition( maxPos );
        Activate();
    }
    else if ( maxVal > -1.0 )
    {
        LOG_INFO(QObject::tr("Failed to relocalise - max at %1 %2 (score: %3).").arg(maxPos.x)
                                                                                .arg(maxPos.y)
                                                                                .arg(maxVal));
    }
}

/**
 Resolve ambiguity in tracked orientation by comparing old and new robot angles.
 **/
bool KltTracker::HasFlipped( float angle, float oldAngle, float threshold )
{
    float diff = Angles::DiffAngle( angle, oldAngle );

    if ( fabsf(diff) > threshold )
    {
        return true;
    }

    return false;
}

void KltTracker::SaveResult( CvPoint2D32f& pos, const float angle, const float error )
{
    if(pos.x < 0)
        pos.x = 0;

    if(pos.x > m_currImg->width)
        pos.x = m_currImg->width;

    if(pos.y < 0)
        pos.y = 0;

    if(pos.y > m_currImg->height)
        pos.y = m_currImg->height;

    m_pos = pos;
    m_angle = angle;
    m_error = error;
}

void KltTracker::SetParam( paramType param, float value )
{
    switch (param)
    {
        case PARAM_NCC_THRESHOLD:
            m_nccThresh = value;
            break;
    }
}
