/***************************************************************************
 *   Copyright (C) 2017 by Nicolas Carion                                  *
 *   This file is part of Kdenlive. See www.kdenlive.org.                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3 or any later version accepted by the       *
 *   membership of KDE e.V. (or its successor approved  by the membership  *
 *   of KDE e.V.), which shall act as a proxy defined in Section 14 of     *
 *   version 3 of the license.                                             *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#include "trackmodel.hpp"
#include "timelinemodel.hpp"
#include "clipmodel.hpp"
#include "snapmodel.hpp"
#include <QDebug>




TrackModel::TrackModel(std::weak_ptr<TimelineModel> parent, int id) :
    m_parent(parent)
    , m_id(id == -1 ? TimelineModel::getNextId() : id)
{
}

int TrackModel::construct(std::weak_ptr<TimelineModel> parent, int id, int pos)
{
    std::shared_ptr<TrackModel> track(new TrackModel(parent, id));
    id = track->m_id;
    if (auto ptr = parent.lock()) {
        ptr->registerTrack(std::move(track), pos);
    } else {
        qDebug() << "Error : construction of track failed because parent timeline is not available anymore";
        Q_ASSERT(false);
    }
    return id;
}

int TrackModel::getClipsCount()
{
    int count = 0;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < m_playlists[j].count(); i++) {
            if (!m_playlists[j].is_blank(i)) {
                count++;
            }
        }
    }
    Q_ASSERT(count == static_cast<int>(m_allClips.size()));
    return count;
}

Fun TrackModel::requestClipInsertion_lambda(int cid, int position)
{
    // By default, insertion occurs in topmost track
    // Find out the clip id at position
    int target_clip = m_playlists[0].get_clip_index_at(position);
    int count = m_playlists[0].count();

    //we create the function that has to be executed after the melt order. This is essentially book-keeping
    auto end_function = [cid, this, position]() {
        if (auto ptr = m_parent.lock()) {
            std::shared_ptr<ClipModel> clip = ptr->getClipPtr(cid);
            m_allClips[clip->getId()] = clip;  //store clip
            //update clip position and track
            clip->setPosition(position);
            clip->setCurrentTrackId(getId());
            int new_in = clip->getPosition();
            int new_out = new_in + clip->getPlaytime() - 1;
            ptr->m_snaps->addPoint(new_in);
            ptr->m_snaps->addPoint(new_out);
            return true;
        } else {
            qDebug() << "Error : Clip Insertion failed because timeline is not available anymore";
            return false;
        }
    };
    if (target_clip >= count && isBlankAt(position)) {
        //In that case, we append after, in the first playlist
        return [this, position, cid, end_function]() {
            if (auto ptr = m_parent.lock()) {
                std::shared_ptr<ClipModel> clip = ptr->getClipPtr(cid);
                int index = m_playlists[0].insert_at(position, *clip, 1);
                return index != -1 && end_function();
            } else {
                qDebug() << "Error : Clip Insertion failed because timeline is not available anymore";
                return false;
            }
        };
    } else {
        if (isBlankAt(position)) {
            int blank_end = getBlankEnd(position);
            int length = -1;
            if (auto ptr = m_parent.lock()) {
                std::shared_ptr<ClipModel> clip = ptr->getClipPtr(cid);
                length = clip->getPlaytime();
            }
            if (blank_end >= position + length) {
                return [this, position, cid, end_function]() {
                    if (auto ptr = m_parent.lock()) {
                        std::shared_ptr<ClipModel> clip = ptr->getClipPtr(cid);
                        int index = m_playlists[0].insert_at(position, *clip, 1);
                        return index != -1 && end_function();
                    } else {
                        qDebug() << "Error : Clip Insertion failed because timeline is not available anymore";
                        return false;
                    }
                };
            }
        }
    }
    return [](){return false;};
}

bool TrackModel::requestClipInsertion(int cid, int position, Fun& undo, Fun& redo)
{
    auto operation = requestClipInsertion_lambda(cid, position);
    if (operation()) {
        auto reverse = requestClipDeletion_lambda(cid);
        UPDATE_UNDO_REDO(operation, reverse, undo, redo);
        return true;
    }
    return false;
}

Fun TrackModel::requestClipDeletion_lambda(int cid)
{
    //Find index of clip
    int clip_position = m_allClips[cid]->getPosition();
    int old_in = clip_position;
    int old_out = old_in + m_allClips[cid]->getPlaytime() - 1;
    return [clip_position, cid, old_in, old_out, this]() {
        auto clip_loc = getClipIndexAt(clip_position);
        int target_track = clip_loc.first;
        int target_clip = clip_loc.second;
        Q_ASSERT(target_clip < m_playlists[target_track].count());
        Q_ASSERT(!m_playlists[target_track].is_blank(target_clip));
        auto prod = m_playlists[target_track].replace_with_blank(target_clip);
        if (prod != nullptr) {
            m_playlists[target_track].consolidate_blanks();
            m_allClips[cid]->setCurrentTrackId(-1);
            m_allClips.erase(cid);
            delete prod;
            if (auto ptr = m_parent.lock()) {
                ptr->m_snaps->removePoint(old_in);
                ptr->m_snaps->removePoint(old_out);
            }
            return true;
        }
        return false;
    };
}

bool TrackModel::requestClipDeletion(int cid, Fun& undo, Fun& redo)
{
    Q_ASSERT(m_allClips.count(cid) > 0);
    auto old_clip = m_allClips[cid];
    int old_position = old_clip->getPosition();
    auto operation = requestClipDeletion_lambda(cid);
    if (operation()) {
        auto reverse = requestClipInsertion_lambda(cid, old_position);
        UPDATE_UNDO_REDO(operation, reverse, undo, redo);
        return true;
    }
    return false;
}

int TrackModel::getBlankSizeNearClip(int cid, bool after)
{
    Q_ASSERT(m_allClips.count(cid) > 0);
    int clip_position = m_allClips[cid]->getPosition();
    auto clip_loc = getClipIndexAt(clip_position);
    int track = clip_loc.first;
    int index = clip_loc.second;
    int other_index; //index in the other track
    int other_track = (track+1)%2;
    if (after) {
        int first_pos = m_playlists[track].clip_start(index) + m_playlists[track].clip_length(index);
        other_index = m_playlists[other_track].get_clip_index_at(first_pos);
        index++;
    } else {
        int last_pos = m_playlists[track].clip_start(index) - 1;
        other_index = m_playlists[other_track].get_clip_index_at(last_pos);
        index--;
    }
    if (index < 0) return 0;
    int length = INT_MAX;
    if (index < m_playlists[track].count()) {
        if (!m_playlists[track].is_blank(index)) {
            return 0;
        }
        length = std::min(length, m_playlists[track].clip_length(index));
    }
    if (other_index < m_playlists[other_track].count()) {
        if (!m_playlists[other_track].is_blank(other_index)) {
            return 0;
        }
        length = std::min(length, m_playlists[other_track].clip_length(other_index));
    }
    return length;
}

Fun TrackModel::requestClipResize_lambda(int cid, int in, int out, bool right)
{
    int clip_position = m_allClips[cid]->getPosition();
    int old_in = clip_position;
    int old_out = old_in + m_allClips[cid]->getPlaytime() - 1;
    auto clip_loc = getClipIndexAt(clip_position);
    int target_track = clip_loc.first;
    int target_clip = clip_loc.second;
    Q_ASSERT(target_clip < m_playlists[target_track].count());
    int size = out - in + 1;

    auto update_snaps = [old_in, old_out, this](int new_in, int new_out) {
        if (auto ptr = m_parent.lock()) {
            ptr->m_snaps->removePoint(old_in);
            ptr->m_snaps->removePoint(old_out);
            ptr->m_snaps->addPoint(new_in);
            ptr->m_snaps->addPoint(new_out);
        } else {
            qDebug() << "Error : clip resize failed because parent timeline is not available anymore";
            Q_ASSERT(false);
        }
    };

    int delta = m_allClips[cid]->getPlaytime() - size;
    if (delta == 0) {
        return [](){return true;};
    }
    if (delta > 0) { //we shrink clip
        return [right, target_clip, target_track, clip_position, delta, in, out, cid, update_snaps, this](){
            int target_clip_mutable = target_clip;
            int blank_index = right ? (target_clip_mutable + 1) : target_clip_mutable;
            // insert blank to space that is going to be empty
            // The second is parameter is delta - 1 because this function expects an out time, which is basically size - 1
            m_playlists[target_track].insert_blank(blank_index, delta - 1);
            if (!right) {
                m_allClips[cid]->setPosition(clip_position + delta);
                //Because we inserted blank before, the index of our clip has increased
                target_clip_mutable++;
            }
            int err = m_playlists[target_track].resize_clip(target_clip_mutable, in, out);
            //make sure to do this after, to avoid messing the indexes
            m_playlists[target_track].consolidate_blanks();
            if (err == 0) {
                update_snaps(m_allClips[cid]->getPosition(), m_allClips[cid]->getPosition() + out - in);
            }
            return err == 0;
        };
    } else {
        int blank = -1;
        int other_blank_end = getBlankEnd(clip_position, (target_track + 1) % 2);
        if (right) {
            if (target_clip == m_playlists[target_track].count() - 1 && other_blank_end >= out) {
                //clip is last, it can always be extended
                return [this, target_clip, target_track, in, out, update_snaps, cid]() {
                    int err = m_playlists[target_track].resize_clip(target_clip, in, out);
                    if (err == 0) {
                        update_snaps(m_allClips[cid]->getPosition(), m_allClips[cid]->getPosition() + out - in);
                    }
                    return err == 0;
                };
            }

            blank = target_clip + 1;
        } else {
            if (target_clip == 0) {
                //clip is first, it can never be extended on the left
                return [](){return false;};
            }
            blank = target_clip - 1;
        }
        if (m_playlists[target_track].is_blank(blank)) {
            int blank_length = m_playlists[target_track].clip_length(blank);
            if (blank_length + delta >= 0 && other_blank_end >= out) {
                return [blank_length, blank, right, cid, delta, update_snaps, this, in, out, target_clip, target_track](){
                    int target_clip_mutable = target_clip;
                    int err = 0;
                    if (blank_length + delta == 0) {
                        err = m_playlists[target_track].remove(blank);
                        if (!right) {
                            target_clip_mutable--;
                        }
                    } else {
                        err = m_playlists[target_track].resize_clip(blank, 0, blank_length + delta - 1);
                    }
                    if (err == 0) {
                        err = m_playlists[target_track].resize_clip(target_clip_mutable, in, out);
                    }
                    if (!right && err == 0) {
                        m_allClips[cid]->setPosition(m_playlists[target_track].clip_start(target_clip_mutable));
                    }
                    if (err == 0) {
                        update_snaps(m_allClips[cid]->getPosition(), m_allClips[cid]->getPosition() + out - in);
                    }
                    return err == 0;
                };
            }
        }
    }
    return [](){return false;};
}

int TrackModel::getId() const
{
    return m_id;
}

int TrackModel::getClipByRow(int row) const
{
    if (row >= static_cast<int>(m_allClips.size())) {
        return -1;
    }
    auto it = m_allClips.cbegin();
    std::advance(it, row);
    return (*it).first;
}

int TrackModel::getRowfromClip(int cid) const
{
    Q_ASSERT(m_allClips.count(cid) > 0);
    return (int)std::distance(m_allClips.begin(), m_allClips.find(cid));
}

QVariant TrackModel::getProperty(const QString &name)
{
    return m_track.get(name.toUtf8().constData());
}

void TrackModel::setProperty(const QString &name, const QString &value)
{
    m_track.set(name.toUtf8().constData(), value.toUtf8().constData());
}

bool TrackModel::checkConsistency()
{
    auto ptr = m_parent.lock();
    if (!ptr) {
        return false;
    }
    std::vector<std::pair<int, int> > clips; //clips stored by (position, id)
    for (const auto& c : m_allClips) {
        Q_ASSERT(c.second);
        Q_ASSERT(c.second.get() == ptr->getClipPtr(c.first).get());
        clips.push_back({c.second->getPosition(), c.first});
    }
    std::sort(clips.begin(), clips.end());
    size_t current_clip = 0;
    int playtime = std::max(m_playlists[0].get_playtime(), m_playlists[1].get_playtime());
    for(int i = 0; i < playtime; i++) {
        int track, index;
        if (isBlankAt(i)) {
            track = 0;
            index = m_playlists[0].get_clip_index_at(i);
        } else {
            auto clip_loc = getClipIndexAt(i);
            track = clip_loc.first;
            index = clip_loc.second;
        }
        Q_ASSERT(m_playlists[(track+1)%2].is_blank_at(i));
        if (current_clip < clips.size() && i >= clips[current_clip].first) {
            auto clip = m_allClips[clips[current_clip].second];
            if (i >= clips[current_clip].first + clip->getPlaytime()) {
                current_clip++;
                i--;
                continue;
            } else {
                if (isBlankAt(i)) {
                    qDebug() << "ERROR: Found blank when clip was required at position " << i;
                    return false;
                }
                auto pr = m_playlists[track].get_clip(index);
                Mlt::Producer prod(pr);
                if (!prod.same_clip(*clip)) {
                    qDebug() << "ERROR: Wrong clip at position " << i;
                    delete pr;
                    return false;
                }
                delete pr;
            }
        } else {
            if (!isBlankAt(i)) {
                qDebug() << "ERROR: Found clip when blank was required at position " << i;
                return false;
            }
        }
    }
    return true;
}

std::pair<int, int> TrackModel::getClipIndexAt(int position)
{
    for (int j = 0; j < 2; j++) {
        if (! m_playlists[j].is_blank_at(position)) {
            return {j, m_playlists[j].get_clip_index_at(position)};
        }
    }
    Q_ASSERT(false);
    return {-1,-1};
}

bool TrackModel::isBlankAt(int position)
{
    return m_playlists[0].is_blank_at(position) && m_playlists[1].is_blank_at(position);
}

int TrackModel::getBlankEnd(int position, int track)
{
    Q_ASSERT(m_playlists[track].is_blank_at(position));
    int clip_index = m_playlists[track].get_clip_index_at(position);
    int count = m_playlists[track].count();
    if (clip_index < count) {
        int blank_start = m_playlists[track].clip_start(clip_index);
        int blank_length = m_playlists[track].clip_length(clip_index);
        return blank_start + blank_length;
    }
    return INT_MAX;

}
int TrackModel::getBlankEnd(int position)
{
    int end = INT_MAX;
    for (int j = 0; j < 2; j++) {
        end = std::min(getBlankEnd(position, j), end);
    }
    return end;
}