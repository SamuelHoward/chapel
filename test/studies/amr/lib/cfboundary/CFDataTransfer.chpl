
use FineBoundaryArray_def;



//|\"""""""""""""""""""""""""""""""""|\
//| >    GridArray.coarsen_Linear    | >
//|/_________________________________|/
//----------------------------------------------------------------
// Returns values from the GridArray linearly interpolated to the 
// input domain coarse_cells.  This is akin to accessing the
// GridArray's values on a coarser index space.
//----------------------------------------------------------------
def GridArray.coarsen_Linear(
  coarse_cells: domain(dimension,stridable=true),
  ref_ratio:    dimension*int)
{

  //==== Volume fraction is 1/product(ref_ratio) ====
  var volume_fraction: real = 1.0;
  for d in dimensions do
    volume_fraction /= ref_ratio(d):real;


  //==== Compute coarse averages ====
  var coarse_values: [coarse_cells] real;

  forall coarse_cell in coarse_cells {
    var fine_cells = refine(coarse_cell, ref_ratio);
    for fine_cell in fine_cells do
      coarse_values(coarse_cell) += value(fine_cell);
    coarse_values(coarse_cell) *= volume_fraction;
  }

  return coarse_values;

}
// /|"""""""""""""""""""""""""""""""""/|
//< |    GridArray.coarsen_Linear    < |
// \|_________________________________\|




//|\""""""""""""""""""""""""""""""""|\
//| >    GridArray.refine_Linear    | >
//|/________________________________|/
//----------------------------------------------------------------
// Returns values from the GridArray linearly interpolated to the 
// input domain fine_cells.  This is akin to accessing the
// GridArray's values on a finer index space.
//----------------------------------------------------------------
def GridArray.refine_Linear(
  fine_cells: domain(dimension,stridable=true),
  ref_ratio:  dimension*int)
{

  var coarse_cells  = grid.cells( coarsen(fine_cells, ref_ratio) );

  var coarse_values = value(coarse_cells);
  var coarse_diffs: [coarse_cells] [dimensions] real;


  //===> Form interpolant data (values and differentials ===>
  forall cell in coarse_cells {
    var diff_mag, diff_sign, diff_low, diff_high, diff_cen: real;
    var shift: dimension*int;

    for d in dimensions {
      shift *= 0;
      shift(d) = 2;

      diff_low  = value(cell) - value(cell-shift);
      diff_high = value(cell+shift) - value(cell);
      diff_cen  = (diff_high - diff_low) / 2.0;

      if diff_low*diff_high > 0 {
        diff_sign = diff_low / abs(diff_low);
        diff_mag = min( abs(diff_low), abs(diff_high), abs(diff_cen) );
        coarse_diffs(cell)(d) = diff_sign * diff_mag;
      }
      else
        coarse_diffs(cell)(d) = 0.0;
    }
  }
  //<=== Form interpolant data (values and differentials) <===


  //===> Evaluate interpolant on fine cells ===>
  var fine_values: [fine_cells] real;

  forall fine_cell in fine_cells {
    var coarse_cell = coarsen(fine_cell, ref_ratio);
    fine_values(fine_cell) = coarse_values(coarse_cell);

    var fine_displacement: real = 0.0;

    for d in dimensions {
      //==== Move to coarse indices ====
      fine_displacement = fine_cell(d):real / ref_ratio(d):real;

      //==== Compute displacement ====
      fine_displacement -= coarse_cell(d):real;

      //==== Rescale: One cell occupies 2 indices ====
      fine_displacement /= 2.0;

      //==== Modify fine_value ====
      fine_values(fine_cell) += fine_displacement * coarse_diffs(coarse_cell)(d);
    }      

  }
  //<=== Evaluate interpolant on fine cells <===


  return fine_values;

}
// /|""""""""""""""""""""""""""""""""/|
//< |    GridArray.refine_Linear    < |
// \|________________________________\|




//|\"""""""""""""""""""""""""""""""""""""""|\
//| >    LevelArray.getFineValues_Linear   | >
//|/_______________________________________|/
def LevelArray.getFineValues_Linear(
  q_fine:      LevelArray,
  cf_boundary: CFBoundary)
{

  //==== Safety check ====
  assert(this.level == cf_boundary.coarse_level);
  assert(q_fine.level == cf_boundary.fine_level);
  
  //==== Refinement ratio ====
  const ref_ratio = refinementRatio(this.level, q_fine.level);

  //==== Each grid obtains coarsened values from fine neighbors ====
  for grid in this.level.grids {
    for (fine_nbr,overlap) in cf_boundary.fine_overlaps(grid) do
      this(grid,overlap) = q_fine(fine_nbr).coarsen_Linear(overlap,ref_ratio);
  }

}
// /|""""""""""""""""""""""""""""""""""""""""/|
//< |    LevelArray.getFineValues_Linear    < |
// \|________________________________________\|





//|\"""""""""""""""""""""""""""""""""""""""""""""""""|\
//| >    FineBoundaryArray.getCoarseValues_Linear    | >
//|/_________________________________________________|/
def FineBoundaryArray.getCoarseValues_Linear(
  q_coarse:  LevelArray)
{

  //==== Pull refinement ratio ====
  const ref_ratio = cf_boundary.ref_ratio;

  //==== Prepare ghost data of q_coarse ====
  q_coarse.extrapolateGhostData();
  q_coarse.fillOverlapRegions();

  //===> Interpolate on each fine grid ===>
  for grid in cf_boundary.fine_level.grids {
    
    var arrays_to_fill = array_sets(fine_grid);
    
    //--------------------------------------------------------------------
    // At the moment, it seems like it's actually easier to compare every
    // array's domain to coarse_nbr.cells, rather than trying to use the
    // DomainSet of sharply-defined overlap data.
    //--------------------------------------------------------------------
    for coarse_nbr in cf_boundary.coarse_overlaps(grid).neighbors {

      var refined_coarse_cells = refine(coarse_nbr.cells,ref_ratio);

      //===> If array's domain overlaps coarse_nbr, then interpolate ===>
      for array in arrays_to_fill {
        var overlap = array.dom( refined_coarse_cells );
        
        if overlap.numIndices>0 then
          array.value(overlap) =
              q_coarse(coarse_nbr).refine_Linear(overlap,ref_ratio);
      }
      //<=== If array's domain overlaps coarse_nbr, then interpolate <===
    }
    
  }
  //<=== Interpolate on each fine grid <===

}
// /|"""""""""""""""""""""""""""""""""""""""""""""""""/|
//< |    FineBoundaryArray.getCoarseValues_Linear    < |
// \|_________________________________________________\|
